// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimGraphEditBase.h"
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"

#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Subsystems/AssetEditorSubsystem.h"
#include "BlueprintEditor.h"
#include "Editor.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static Data
// ============================================================================

const TCHAR* ClaireonAnimGraphEditToolBase::AnimGraphSessionToolName = TEXT("editor.animgraph.edit");
TMap<FString, FAnimGraphEditToolData> ClaireonAnimGraphEditToolBase::ToolData;
bool ClaireonAnimGraphEditToolBase::bDelegateRegistered = false;

void ClaireonAnimGraphEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	ToolData.Remove(Info.SessionId);
}

void ClaireonAnimGraphEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonAnimGraphEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

// ============================================================================
// RequireSession
// ============================================================================

bool ClaireonAnimGraphEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FAnimGraphEditToolData*& OutData,
	FToolResult& OutError)
{
	if (!Arguments->TryGetStringField(TEXT("session_id"), OutSessionId))
	{
		OutError = MakeErrorResult(TEXT("Missing required field: session_id"));
		return false;
	}

	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(OutSessionId);
	if (!Session)
	{
		OutError = MakeErrorResult(FString::Printf(TEXT("Session not found or expired: %s"), *OutSessionId));
		return false;
	}
	Session->Touch();

	OutData = ToolData.Find(OutSessionId);
	if (!OutData || !OutData->IsValid())
	{
		OutError = MakeErrorResult(TEXT("Session tool data not found or AnimBP no longer valid. Use animgraph_open to start a new session."));
		return false;
	}

	// Update response mode if provided
	FString ResponseMode;
	if (Arguments->TryGetStringField(TEXT("response_mode"), ResponseMode))
	{
		OutData->ResponseMode = ResponseMode;
	}

	// Clear per-operation state
	OutData->LastOperationAffectedNodes.Empty();
	OutData->GuidCorrections.Empty();

	// Snapshot for "changed" mode
	if (OutData->ResponseMode == TEXT("changed"))
	{
		SnapshotPinConnections(OutData);
	}

	return true;
}

// ============================================================================
// SnapshotPinConnections
// ============================================================================

void ClaireonAnimGraphEditToolBase::SnapshotPinConnections(FAnimGraphEditToolData* Data)
{
	Data->PreOpPinConnections.Empty();
	UEdGraph* Graph = Data->CurrentGraph.Get();
	if (!Graph) return;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node) continue;
		TMap<FName, TArray<FString>> NodePins;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->LinkedTo.Num() == 0) continue;
			TArray<FString> ConnectedTitles;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked && Linked->GetOwningNode())
				{
					ConnectedTitles.Add(Linked->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString());
				}
			}
			NodePins.Add(Pin->PinName, MoveTemp(ConnectedTitles));
		}
		if (NodePins.Num() > 0)
		{
			Data->PreOpPinConnections.Add(Node->NodeGuid, MoveTemp(NodePins));
		}
	}
}

// ============================================================================
// BuildStateResponse
// ============================================================================

FToolResult ClaireonAnimGraphEditToolBase::BuildStateResponse(const FString& SessionId, FAnimGraphEditToolData* Data)
{
	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();
	if (!AnimBP || !Graph)
	{
		return MakeErrorResult(TEXT("AnimBP or Graph no longer valid"));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetStringField(TEXT("asset_path"), AnimBP->GetPathName());
	Result->SetStringField(TEXT("current_graph"), Graph->GetName());
	Result->SetStringField(TEXT("status"), Data->Cursor.LastOperationStatus);

	// Cursor info
	TSharedPtr<FJsonObject> CursorObj = MakeShared<FJsonObject>();
	CursorObj->SetStringField(TEXT("focused_node_guid"), Data->Cursor.FocusedNodeGuid.ToString());
	CursorObj->SetStringField(TEXT("focused_pin"), Data->Cursor.FocusedPinName.ToString());
	CursorObj->SetStringField(TEXT("graph_name"), Data->Cursor.GraphName);
	Result->SetObjectField(TEXT("cursor"), CursorObj);

	// Available graphs
	TArray<ClaireonAnimGraphHelpers::FAnimGraphInfo> AllGraphs = ClaireonAnimGraphHelpers::CollectAllGraphs(AnimBP);
	TArray<TSharedPtr<FJsonValue>> GraphsArray;
	for (const auto& Info : AllGraphs)
	{
		TSharedPtr<FJsonObject> GObj = MakeShared<FJsonObject>();
		GObj->SetStringField(TEXT("name"), Info.Name);
		GObj->SetStringField(TEXT("type"), Info.Type);
		GObj->SetNumberField(TEXT("node_count"), Info.NodeCount);
		if (!Info.ParentGraphName.IsEmpty())
		{
			GObj->SetStringField(TEXT("parent"), Info.ParentGraphName);
		}
		GraphsArray.Add(MakeShared<FJsonValueObject>(GObj));
	}
	Result->SetArrayField(TEXT("available_graphs"), GraphsArray);

	// Status-only mode
	if (Data->ResponseMode == TEXT("status"))
	{
		return MakeSuccessResult(Result, Data->Cursor.LastOperationStatus);
	}

	// Serialize nodes
	TArray<TSharedPtr<FJsonValue>> NodesArray;
	const FString DetailLevel = TEXT("full");

	if (Data->ResponseMode == TEXT("changed") && Data->LastOperationAffectedNodes.Num() > 0)
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			if (Data->LastOperationAffectedNodes.Contains(Node->NodeGuid))
			{
				TSharedPtr<FJsonObject> NodeObj = ClaireonAnimGraphHelpers::SerializeAnimGraphNode(Node, DetailLevel, AnimBP);
				if (NodeObj) NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
			}
		}
	}
	else
	{
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node) continue;
			TSharedPtr<FJsonObject> NodeObj = ClaireonAnimGraphHelpers::SerializeAnimGraphNode(Node, DetailLevel, AnimBP);
			if (NodeObj) NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
		}
	}

	Result->SetArrayField(TEXT("nodes"), NodesArray);
	Result->SetNumberField(TEXT("node_count"), NodesArray.Num());
	Result->SetNumberField(TEXT("total_nodes_in_graph"), Graph->Nodes.Num());

	// GUID corrections
	if (Data->GuidCorrections.Num() > 0)
	{
		TSharedPtr<FJsonObject> Corrections = MakeShared<FJsonObject>();
		for (const auto& Pair : Data->GuidCorrections)
		{
			Corrections->SetStringField(Pair.Key.ToString(), Pair.Value.ToString());
		}
		Result->SetObjectField(TEXT("guid_corrections"), Corrections);
	}

	FString SessionHintSummaryTag;
	ClaireonAssetUtils::EmitSessionHintIfNeeded(Result, Data->ConsecutiveAssetPathCalls, AnimBP->GetPathName(), SessionId, SessionHintSummaryTag);

	return MakeSuccessResult(Result, Data->Cursor.LastOperationStatus + SessionHintSummaryTag);
}

// ============================================================================
// RefreshBlueprintEditorInPlace
// ============================================================================

void ClaireonAnimGraphEditToolBase::RefreshBlueprintEditorInPlace(UBlueprint* Blueprint)
{
	if (!Blueprint || !GEditor) return;

	UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
	if (!Subsystem) return;

	IAssetEditorInstance* EditorInstance = Subsystem->FindEditorForAsset(Blueprint, false);
	if (!EditorInstance) return;

	// Cast to FBlueprintEditor and refresh in-place
	FBlueprintEditor* BPEditor = static_cast<FBlueprintEditor*>(EditorInstance);
	if (BPEditor)
	{
		BPEditor->RefreshEditors();
	}
}
