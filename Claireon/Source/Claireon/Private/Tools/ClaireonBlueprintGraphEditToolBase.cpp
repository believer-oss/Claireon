// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"
#include "ClaireonLog.h"
#include "ClaireonBlueprintHelpers.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "EdGraphUtilities.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

TMap<FString, FBlueprintEditToolData> ClaireonBlueprintGraphEditToolBase::ToolData;
bool ClaireonBlueprintGraphEditToolBase::bDelegateRegistered = false;

// ============================================================================
// Session Delegate
// ============================================================================

void ClaireonBlueprintGraphEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("claireon.blueprint_edit_graph"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

// ============================================================================
// Session / cursor helpers
// ============================================================================

FString ClaireonBlueprintGraphEditToolBase::BuildAvailableGraphsList(const UBlueprint* Blueprint) const
{
	TArray<FString> Names;
	if (Blueprint)
	{
		for (UEdGraph* Graph : Blueprint->UbergraphPages)       { if (Graph) Names.Add(Graph->GetName()); }
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)       { if (Graph) Names.Add(Graph->GetName()); }
		for (UEdGraph* Graph : Blueprint->MacroGraphs)          { if (Graph) Names.Add(Graph->GetName()); }
		for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs) { if (Graph) Names.Add(Graph->GetName()); }
	}
	if (Names.Num() == 0)
	{
		return TEXT("(no graphs)");
	}
	return FString::Join(Names, TEXT(", "));
}

FToolResult ClaireonBlueprintGraphEditToolBase::BuildStateResponse(const FString& SessionId, FBlueprintEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Invalid session"));
	}

	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	// Validate cursor
	ValidateCursor(Data);

	// =========================================================================
	// Determine effective response mode
	// =========================================================================
	// Legacy: bSuppressOutput maps to "status"
	FString EffectiveMode = Data->ResponseMode;
	if (Data->bSuppressOutput && EffectiveMode == TEXT("changed"))
	{
		EffectiveMode = TEXT("status");
	}

	// "changed" falls back to "status" when no affected nodes were recorded
	// (e.g. non-connectivity ops like save/format, or open which bypasses this path)
	if (EffectiveMode == TEXT("changed") && Data->LastOperationAffectedNodes.IsEmpty())
	{
		EffectiveMode = TEXT("status");
	}

	// =========================================================================
	// Structured response data (session_id contract: callers read Data.session_id
	// instead of grepping the Summary's "Session ID:" line).
	// See CLAIREON_BP_SESSION_ID_PROPOSAL.md (#0000).
	// =========================================================================
	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("asset_path"),
		Data->Blueprint.IsValid() ? Data->Blueprint->GetPathName() : FString());
	ResponseData->SetStringField(TEXT("graph_name"),
		Data->Graph.IsValid() ? Data->Graph->GetName() : FString());
	ResponseData->SetStringField(TEXT("response_mode"), EffectiveMode);

	// =========================================================================
	// Nudge toward explicit open/close discipline after sustained asset_path use.
	// Threshold: > 5 consecutive auto-opens. Cadence: first hint at call 6, then
	// every 5 past that (11, 16, ...). Counter resets whenever the caller passes
	// session_id. See CLAIREON_BP_SESSION_ID_PROPOSAL.md (#0000).
	// =========================================================================
	FString SessionHintSummaryTag;
	if (Data->ConsecutiveAssetPathCalls > 5 && Data->ConsecutiveAssetPathCalls % 5 == 1)
	{
		const FString HintText = FString::Printf(
			TEXT("You've called this tool %d times in a row with asset_path and no session_id. ")
			TEXT("This session (id=%s) is still locked on '%s' and will not release until the ")
			TEXT("configured idle timeout elapses. For multi-step edits, call operation='open' ")
			TEXT("once, read Data.session_id from the response, and pass it on subsequent calls. ")
			TEXT("Call operation='close' when done to release the lock immediately."),
			Data->ConsecutiveAssetPathCalls,
			*SessionId,
			Data->Blueprint.IsValid() ? *Data->Blueprint->GetPathName() : TEXT("<unknown>"));
		ResponseData->SetStringField(TEXT("session_hint"), HintText);
		SessionHintSummaryTag = FString::Printf(
			TEXT("\n\n[hint] %d consecutive asset_path calls -- consider explicit session_id (session=%s)."),
			Data->ConsecutiveAssetPathCalls,
			*SessionId);
	}

	// =========================================================================
	// Surface GUID corrections so the MCP client can update stale references
	// =========================================================================
	FString GuidCorrectionNote;
	if (Data->GuidCorrections.Num() > 0)
	{
		GuidCorrectionNote = TEXT("\n\n## GUID Corrections (blueprint was recompiled — update your references)\n");
		for (const auto& Pair : Data->GuidCorrections)
		{
			GuidCorrectionNote += FString::Printf(TEXT("  %s → %s\n"),
				*Pair.Key.ToString(), *Pair.Value.ToString());
		}
		Data->GuidCorrections.Empty();
	}

	// =========================================================================
	// "status" mode — brief status line only
	// =========================================================================
	if (EffectiveMode == TEXT("status"))
	{
		FString StatusMsg = Data->Cursor.LastOperationStatus.IsEmpty() ? TEXT("ok") : FString::Printf(TEXT("ok: %s"), *Data->Cursor.LastOperationStatus);
		return MakeSuccessResult(ResponseData, StatusMsg + GuidCorrectionNote + SessionHintSummaryTag);
	}

	// =========================================================================
	// "changed" mode — pin-level diff of affected nodes
	// =========================================================================
	if (EffectiveMode == TEXT("changed"))
	{
		int32 TotalNodes = Graph->Nodes.Num();
		int32 AffectedCount = Data->LastOperationAffectedNodes.Num();

		FString DiffText;

		// Status header
		if (!Data->Cursor.LastOperationStatus.IsEmpty())
		{
			DiffText += FString::Printf(TEXT("## Status\n%s\n\n"), *Data->Cursor.LastOperationStatus);
		}

		DiffText += FString::Printf(TEXT("## Changed nodes (%d of %d):\n\n"), AffectedCount, TotalNodes);

		for (const FGuid& AffGuid : Data->LastOperationAffectedNodes)
		{
			// Find the node in the current graph
			UEdGraphNode* AffNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, AffGuid);
			if (!AffNode)
			{
				// Node was removed — we can't show its current state; skip
				continue;
			}

			FString NodeTitle = AffNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			FString NodeClass = AffNode->GetClass()->GetName();

			DiffText += FString::Printf(TEXT("[%s] (%s) [GUID: %s]\n"),
				*NodeTitle, *NodeClass, *AffGuid.ToString());

			// Per-pin diff
			bool bAnyPinDiff = false;
			const TMap<FName, TArray<FString>>* PrePinMap = Data->PreOpPinConnections.Find(AffGuid);

			for (UEdGraphPin* DiffPin : AffNode->Pins)
			{
				if (!DiffPin)
				{
					continue;
				}

				// Current connections for this pin
				TArray<FString> CurrentConnected;
				for (UEdGraphPin* LinkedDiff : DiffPin->LinkedTo)
				{
					if (LinkedDiff && LinkedDiff->GetOwningNode())
					{
						CurrentConnected.Add(LinkedDiff->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
					}
				}

				// Pre-op connections for this pin
				TArray<FString> PreviousConnected;
				if (PrePinMap)
				{
					const TArray<FString>* PreConns = PrePinMap->Find(DiffPin->PinName);
					if (PreConns)
					{
						PreviousConnected = *PreConns;
					}
				}

				// Find added connections (in current but not in previous)
				for (const FString& CurConn : CurrentConnected)
				{
					if (!PreviousConnected.Contains(CurConn))
					{
						FString DirArrow = (DiffPin->Direction == EGPD_Output) ? TEXT("->") : TEXT("<-");
						DiffText += FString::Printf(TEXT("  ADDED:   %s(%s) %s [%s]\n"),
							*DiffPin->PinName.ToString(),
							*DiffPin->PinType.PinCategory.ToString(),
							*DirArrow,
							*CurConn);
						bAnyPinDiff = true;
					}
				}

				// Find removed connections (in previous but not in current)
				for (const FString& PrevConn : PreviousConnected)
				{
					if (!CurrentConnected.Contains(PrevConn))
					{
						FString DirArrow = (DiffPin->Direction == EGPD_Output) ? TEXT("->") : TEXT("<-");
						if (CurrentConnected.Num() == 0)
						{
							DiffText += FString::Printf(TEXT("  REMOVED: %s(%s) %s [%s] (now unconnected)\n"),
								*DiffPin->PinName.ToString(),
								*DiffPin->PinType.PinCategory.ToString(),
								*DirArrow,
								*PrevConn);
						}
						else
						{
							DiffText += FString::Printf(TEXT("  REMOVED: %s(%s) %s [%s]\n"),
								*DiffPin->PinName.ToString(),
								*DiffPin->PinType.PinCategory.ToString(),
								*DirArrow,
								*PrevConn);
						}
						bAnyPinDiff = true;
					}
				}
			}

			if (!bAnyPinDiff)
			{
				DiffText += TEXT("  (exec connections unchanged)\n");
			}

			DiffText += TEXT("\n");
		}

		DiffText += FString::Printf(
			TEXT("(Full graph: %d nodes. Use editor.blueprint.getGraph to see all.)"),
			TotalNodes);

		return MakeSuccessResult(ResponseData, DiffText + GuidCorrectionNote + SessionHintSummaryTag);
	}

	// =========================================================================
	// "full" mode — full graph state (JSON + T3D); also the fallback
	// =========================================================================
	{
		// Part 1: Operation status + Cursor info + Graph state summary
		FString StatusText;

		// Operation status
		if (!Data->Cursor.LastOperationStatus.IsEmpty())
		{
			StatusText += FString::Printf(TEXT("## Status\n%s\n\n"), *Data->Cursor.LastOperationStatus);
		}

		// Session info
		StatusText += FString::Printf(TEXT("## Session\nSession ID: %s\nBlueprint: %s\nGraph: %s\n\n"),
			*SessionId,
			*Blueprint->GetPathName(),
			*Graph->GetName());

		// Cursor info
		StatusText += TEXT("## Cursor\n");
		if (Data->Cursor.FocusedNodeGuid.IsValid())
		{
			UEdGraphNode* FocusedNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, Data->Cursor.FocusedNodeGuid);
			if (FocusedNode)
			{
				StatusText += FString::Printf(TEXT("Focused Node: %s [GUID: %s]\n"),
					*FocusedNode->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					*Data->Cursor.FocusedNodeGuid.ToString());

				if (Data->Cursor.FocusedPinName != NAME_None)
				{
					UEdGraphPin* FocusedPin = FocusedNode->FindPin(Data->Cursor.FocusedPinName, Data->Cursor.FocusedPinDirection);
					if (FocusedPin)
					{
						FString PinDir = (FocusedPin->Direction == EGPD_Input) ? TEXT("input") : TEXT("output");
						StatusText += FString::Printf(TEXT("Focused Pin: %s (%s, %s)\n"),
							*FocusedPin->PinName.ToString(),
							*PinDir,
							*FocusedPin->PinType.PinCategory.ToString());
					}
				}

				StatusText += FString::Printf(TEXT("Position: (%.0f, %.0f)\n"), FocusedNode->NodePosX, FocusedNode->NodePosY);
			}
			else
			{
				StatusText += TEXT("Focused Node: (invalid)\n");
			}
		}
		else
		{
			StatusText += TEXT("Focused Node: (none)\n");
		}
		StatusText += TEXT("\n");

		// Implemented interfaces (read-only reference pattern from
		// ClaireonTool_GetBlueprintProperties.cpp:214-222).
		StatusText += TEXT("## Interfaces\n");
		int32 InterfaceCount = 0;
		for (const FBPInterfaceDescription& Interface : Blueprint->ImplementedInterfaces)
		{
			if (Interface.Interface)
			{
				StatusText += FString::Printf(TEXT("- %s\n"), *Interface.Interface->GetName());
				++InterfaceCount;
			}
		}
		if (InterfaceCount == 0)
		{
			StatusText += TEXT("(none)\n");
		}
		StatusText += TEXT("\n");

		// Graph state summary
		StatusText += FString::Printf(TEXT("## Graph State: %s (%d nodes)\n\n"), *Graph->GetName(), Graph->Nodes.Num());

		// List nodes with simple format
		int32 NodeIndex = 1;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			bool bIsCursor = (Node->NodeGuid == Data->Cursor.FocusedNodeGuid);

			StatusText += FString::Printf(TEXT("%d. [%s] @ (%.0f, %.0f)%s\n"),
				NodeIndex++,
				*NodeTitle,
				Node->NodePosX,
				Node->NodePosY,
				bIsCursor ? TEXT(" <<<CURSOR>>>") : TEXT(""));

			// Show execution connections (simplified)
			TArray<UEdGraphPin*> ExecOutputs = ClaireonBlueprintHelpers::GetExecPins(Node, false, true);
			for (UEdGraphPin* ExecPin : ExecOutputs)
			{
				if (ExecPin->LinkedTo.Num() > 0)
				{
					for (UEdGraphPin* LinkedPin : ExecPin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode())
						{
							FString LinkedTitle = LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString();
							StatusText += FString::Printf(TEXT("   -> exec -> [%s]\n"), *LinkedTitle);
						}
					}
				}
			}
		}

		// Part 2: T3D export (if nodes exist)
		if (Graph->Nodes.Num() > 0)
		{
			// Convert TArray to TSet for FEdGraphUtilities::ExportNodesToText
			TSet<UObject*> NodeSet;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node)
				{
					NodeSet.Add(Node);
				}
			}

			FString T3DText;
			FEdGraphUtilities::ExportNodesToText(NodeSet, T3DText);

			if (!T3DText.IsEmpty())
			{
				StatusText += FString::Printf(TEXT("\n## T3D Export\n\n```\n%s\n```"), *T3DText);
			}
		}

		return MakeSuccessResult(ResponseData, StatusText + GuidCorrectionNote + SessionHintSummaryTag);
	}
}

void ClaireonBlueprintGraphEditToolBase::ValidateCursor(FBlueprintEditToolData* Data)
{
	if (!Data || !Data->Graph.IsValid())
	{
		return;
	}

	// Check if focused node still exists
	if (Data->Cursor.FocusedNodeGuid.IsValid())
	{
		UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Data->Graph.Get(), Data->Cursor.FocusedNodeGuid);
		if (!Node)
		{
			// Node was deleted, reset cursor to first root node
			TArray<UEdGraphNode*> RootNodes = ClaireonBlueprintHelpers::FindRootNodes(Data->Graph.Get());
			if (RootNodes.Num() > 0)
			{
				Data->Cursor.FocusedNodeGuid = RootNodes[0]->NodeGuid;
				UEdGraphPin* FirstOutput = ClaireonBlueprintHelpers::GetFirstOutputPin(RootNodes[0]);
				if (FirstOutput)
				{
					Data->Cursor.FocusedPinName = FirstOutput->PinName;
					Data->Cursor.FocusedPinDirection = FirstOutput->Direction;
				}
			}
			else
			{
				// No nodes left, reset cursor
				Data->Cursor.FocusedNodeGuid = FGuid();
				Data->Cursor.FocusedPinName = NAME_None;
			}
		}
	}
}

void ClaireonBlueprintGraphEditToolBase::InitToolDataForSession(const FString& SessionId, UBlueprint* Blueprint, UEdGraph* Graph)
{
	FBlueprintEditToolData NewData;
	NewData.Blueprint = Blueprint;
	NewData.Graph = Graph;
	NewData.Cursor.GraphName = Graph->GetName();
	NewData.Cursor.ViewportCenter = FVector2D(0.0f, 0.0f);

	// Find first event node to focus cursor
	TArray<UEdGraphNode*> RootNodes = ClaireonBlueprintHelpers::FindRootNodes(Graph);
	if (RootNodes.Num() > 0)
	{
		UEdGraphNode* FirstNode = RootNodes[0];
		NewData.Cursor.FocusedNodeGuid = FirstNode->NodeGuid;
		UEdGraphPin* FirstOutput = ClaireonBlueprintHelpers::GetFirstOutputPin(FirstNode);
		if (FirstOutput)
		{
			NewData.Cursor.FocusedPinName = FirstOutput->PinName;
			NewData.Cursor.FocusedPinDirection = FirstOutput->Direction;
		}
	}

	ToolData.Add(SessionId, MoveTemp(NewData));
	// Caller is responsible for calling ToolData.Find(SessionId) after this
	// function returns (Add may rehash the map, invalidating any prior pointer).
}

bool ClaireonBlueprintGraphEditToolBase::BeginSessionOp(
	const TSharedPtr<FJsonObject>& Arguments,
	const FString& OperationName,
	TSharedPtr<FJsonObject>& OutParams,
	FString& OutSessionId,
	FBlueprintEditToolData*& OutData,
	FToolResult& OutError)
{
	OutSessionId.Reset();
	OutData = nullptr;

	TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
	if (Params->HasField(TEXT("params")))
	{
		const TSharedPtr<FJsonObject>* NestedObj = nullptr;
		if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
		{
			Params = *NestedObj;
		}
	}

	bool bSuppressOutput = false;
	Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);

	FString ResponseMode = TEXT("changed");
	Arguments->TryGetStringField(TEXT("response_mode"), ResponseMode);
	if (bSuppressOutput && !Arguments->HasField(TEXT("response_mode")))
	{
		ResponseMode = TEXT("status");
	}

	if (!ResolveOrOpenSession(Params, OperationName, OutSessionId, OutData, OutError))
	{
		return false;
	}

	FClaireonSessionManager::Get().TouchSession(OutSessionId);
	OutData->bSuppressOutput = bSuppressOutput;
	OutData->ResponseMode = ResponseMode;
	OutData->LastOperationAffectedNodes.Empty();

	OutData->PreOpPinConnections.Empty();
	if (UEdGraph* SnapGraph = OutData->Graph.Get())
	{
		for (UEdGraphNode* SnapNode : SnapGraph->Nodes)
		{
			if (!SnapNode)
			{
				continue;
			}
			TMap<FName, TArray<FString>> PinConns;
			for (UEdGraphPin* SnapPin : SnapNode->Pins)
			{
				if (!SnapPin)
				{
					continue;
				}
				TArray<FString> ConnectedTo;
				for (UEdGraphPin* LinkedPin : SnapPin->LinkedTo)
				{
					if (LinkedPin && LinkedPin->GetOwningNode())
					{
						ConnectedTo.Add(LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
					}
				}
				PinConns.Add(SnapPin->PinName, ConnectedTo);
			}
			OutData->PreOpPinConnections.Add(SnapNode->NodeGuid, PinConns);
		}
	}

	OutParams = Params;
	return true;
}

FToolResult ClaireonBlueprintGraphEditToolBase::CheckMutationAffectedNodes(const FString& OpName, FBlueprintEditToolData* Data, const FToolResult& Result)
{
	if (!Result.bIsError && Data && Data->ResponseMode == TEXT("changed") && Data->LastOperationAffectedNodes.IsEmpty())
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("response_mode=changed: no affected nodes recorded after mutation op '%s' — check handler"),
			*OpName);
	}
	return Result;
}

bool ClaireonBlueprintGraphEditToolBase::ResolveOrOpenSession(
	const TSharedPtr<FJsonObject>& Params,
	const FString& OperationName,
	FString& OutSessionId,
	FBlueprintEditToolData*& OutData,
	FToolResult& OutError)
{
	OutSessionId.Reset();
	OutData = nullptr;

	// Resolution order 1: explicit session_id wins.
	FString SessionId;
	if (Params->TryGetStringField(TEXT("session_id"), SessionId) && !SessionId.IsEmpty())
	{
		FMCPSession* MgrSession = FClaireonSessionManager::Get().FindSession(SessionId);
		if (!MgrSession)
		{
			OutError = MakeErrorResult(FString::Printf(TEXT("Invalid or expired session_id: %s"), *SessionId));
			return false;
		}

		FBlueprintEditToolData* Data = ToolData.Find(SessionId);
		if (!Data)
		{
			OutError = MakeErrorResult(FString::Printf(TEXT("Tool data not found for session_id: %s"), *SessionId));
			return false;
		}

		// Caller used an explicit session_id -- they understand the open/close
		// discipline, so reset the consecutive-auto-open nudge counter.
		Data->ConsecutiveAssetPathCalls = 0;

		OutSessionId = SessionId;
		OutData = Data;
		return true;
	}

	// Resolution order 2: asset_path triggers auto-open (or reuse existing
	// session for the same tool+asset via FClaireonSessionManager::OpenSession).
	FString AssetPath;
	if (Params->TryGetStringField(TEXT("asset_path"), AssetPath) && !AssetPath.IsEmpty())
	{
		// Register delegate on first use (same guard as blueprint_graph_open).
		if (!bDelegateRegistered)
		{
			FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonBlueprintGraphEditToolBase::HandleSessionClosed);
			bDelegateRegistered = true;
		}

		// Canonicalize via the same resolver blueprint_graph_open uses.
		auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
		if (!ResolveResult.bSuccess)
		{
			OutError = MakeErrorResult(ResolveResult.Error);
			return false;
		}
		AssetPath = ResolveResult.ResolvedPath.Path;

		// Load Blueprint (needed to build tool data on a fresh session).
		UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!Blueprint)
		{
			OutError = MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));
			return false;
		}

		// Graph: default to EventGraph, or caller-supplied graph_name. Mirrors
		// blueprint_graph_open's default handling.
		FString GraphName;
		if (!Params->TryGetStringField(TEXT("graph_name"), GraphName))
		{
			GraphName = TEXT("EventGraph");
		}
		UEdGraph* Graph = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, GraphName);
		if (!Graph)
		{
			OutError = MakeErrorResult(FString::Printf(TEXT("Graph '%s' not found in Blueprint %s"), *GraphName, *AssetPath));
			return false;
		}

		// Open (or reuse) a session via the manager.
		double TimeoutMinutes = 60.0;
		Params->TryGetNumberField(TEXT("timeout_minutes"), TimeoutMinutes);
		FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(AssetPath, TEXT("claireon.blueprint_edit_graph"), TimeoutMinutes);

		if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
		{
			const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
			const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
			OutError = MakeErrorResult(FString::Printf(
				TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first, or use mcp_release_sessions(asset_path='%s') to force-release it."),
				*Blocker.ToolName, *Blocker.SessionId,
				static_cast<int32>(Elapsed.GetTotalMinutes()),
				static_cast<int32>(Elapsed.GetTotalSeconds()) % 60,
				*AssetPath));
			return false;
		}

		if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
		{
			OutError = MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
			return false;
		}

		// Success or ReusedExistingSession: populate ToolData if needed.
		const FString& OpenedSessionId = OpenResult.SessionId;
		FBlueprintEditToolData* Data = ToolData.Find(OpenedSessionId);
		if (!Data)
		{
			InitToolDataForSession(OpenedSessionId, Blueprint, Graph);
			Data = ToolData.Find(OpenedSessionId);
			UE_LOG(LogClaireon, Log,
				TEXT("[EditBlueprintGraph] Auto-opened session %s for Blueprint %s (op='%s' asset_path fallback)"),
				*OpenedSessionId, *Blueprint->GetPathName(), *OperationName);
		}

		if (!Data)
		{
			OutError = MakeErrorResult(FString::Printf(TEXT("Failed to initialize tool data for auto-opened session on asset %s"), *AssetPath));
			return false;
		}

		// Track consecutive asset_path resolutions on this session so
		// BuildStateResponse can emit a nudge toward explicit open/close discipline
		// after 5 (first hint at call 6, then every 5 past that).
		Data->ConsecutiveAssetPathCalls++;

		OutSessionId = OpenedSessionId;
		OutData = Data;
		return true;
	}

	// Resolution order 3: neither supplied.
	OutError = MakeErrorResult(FString::Printf(
		TEXT("Missing 'session_id' (or 'asset_path') for operation '%s'. Supply one of: session_id (from a prior open/create) or asset_path (will auto-open a session)."),
		*OperationName));
	return false;
}

#undef LOCTEXT_NAMESPACE
