// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimGraphTools_Session.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "AnimationGraph.h"
#include "ClaireonBlueprintHelpers.h"
#include "EdGraph/EdGraphNode.h"

#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/SavePackage.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

#define LOCTEXT_NAMESPACE "ClaireonAnimGraphTools_Session"

// ============================================================================
// ClaireonAnimGraphTool_Open
// ============================================================================

FString ClaireonAnimGraphTool_Open::GetOperation() const { return TEXT("open"); }

FString ClaireonAnimGraphTool_Open::GetDescription() const
{
	return TEXT("Open a session-based editing session for an Animation Blueprint. Returns session_id for subsequent operations.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the Animation Blueprint to open"), true);
	S.AddString(TEXT("graph_name"), TEXT("Initial graph to focus on (default: root AnimGraph)"));
	S.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'changed' (default), 'full', or 'status'"));
	S.AddNumber(TEXT("timeout_minutes"), TEXT("Session timeout in minutes (default: 60)"));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	// Load AnimBP
	FString LoadError;
	UAnimBlueprint* AnimBP = ClaireonAnimGraphHelpers::LoadAnimBlueprint(AssetPath, LoadError);
	if (!AnimBP)
	{
		return MakeErrorResult(LoadError);
	}

	// Register delegate
	EnsureDelegateRegistered();

	// Open session
	double TimeoutMinutes = 60.0;
	Arguments->TryGetNumberField(TEXT("timeout_minutes"), TimeoutMinutes);
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		AnimBP->GetPathName(), AnimGraphSessionToolName, TimeoutMinutes);

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60));
	}

	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *AnimBP->GetPathName()));
	}

	const FString& SessionId = OpenResult.SessionId;

	// Find initial graph (default to root AnimGraph)
	FString GraphName;
	Arguments->TryGetStringField(TEXT("graph_name"), GraphName);

	UEdGraph* InitialGraph = nullptr;
	if (!GraphName.IsEmpty())
	{
		FString GraphError;
		InitialGraph = ClaireonAnimGraphHelpers::FindAnimGraphByName(AnimBP, GraphName, GraphError);
		if (!InitialGraph)
		{
			FClaireonSessionManager::Get().CloseSession(SessionId);
			return MakeErrorResult(GraphError);
		}
	}
	else
	{
		// Default to first AnimGraph (uses CollectAllGraphs which traverses parent chain for child BPs)
		TArray<ClaireonAnimGraphHelpers::FAnimGraphInfo> AllGraphs = ClaireonAnimGraphHelpers::CollectAllGraphs(AnimBP);
		for (const auto& Info : AllGraphs)
		{
			if (Info.Type == TEXT("AnimGraph"))
			{
				InitialGraph = Info.Graph;
				break;
			}
		}
	}

	if (!InitialGraph)
	{
		FClaireonSessionManager::Get().CloseSession(SessionId);
		return MakeErrorResult(TEXT("No AnimGraph found in this Animation Blueprint"));
	}

	// Create tool data
	FAnimGraphEditToolData NewData;
	NewData.AnimBlueprint = AnimBP;
	NewData.CurrentGraph = InitialGraph;
	NewData.Cursor.GraphName = InitialGraph->GetName();
	NewData.Cursor.ViewportCenter = FVector2D(0.0f, 0.0f);

	// Set response mode
	FString ResponseMode;
	if (Arguments->TryGetStringField(TEXT("response_mode"), ResponseMode))
	{
		NewData.ResponseMode = ResponseMode;
	}
	else
	{
		NewData.ResponseMode = TEXT("full"); // Full on first open
	}

	// Focus cursor on output pose node (if present)
	for (UEdGraphNode* Node : InitialGraph->Nodes)
	{
		if (!Node) continue;
		FString Category = ClaireonAnimGraphHelpers::GetAnimNodeCategory(Node);
		if (Category == TEXT("output_pose"))
		{
			NewData.Cursor.FocusedNodeGuid = Node->NodeGuid;
			break;
		}
	}

	ToolData.Add(SessionId, MoveTemp(NewData));
	FAnimGraphEditToolData* Data = ToolData.Find(SessionId);

	ClaireonAssetUtils::OpenAssetEditorIfHeadless(AnimBP);

	UE_LOG(LogClaireon, Log, TEXT("[AnimGraphEdit] Opened session %s for %s"), *SessionId, *AnimBP->GetPathName());

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Opened %s — graph: %s (%d nodes)"),
		*AnimBP->GetName(), *InitialGraph->GetName(), InitialGraph->Nodes.Num());

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_Close
// ============================================================================

FString ClaireonAnimGraphTool_Close::GetOperation() const { return TEXT("close"); }

FString ClaireonAnimGraphTool_Close::GetDescription() const
{
    return TEXT("Close an animation graph editing session, releasing the asset lock and clearing any in-session state.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID to close"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();

	// No explicit refresh — MarkBlueprintAsModified notifications are sufficient

	ToolData.Remove(SessionId);
	FClaireonSessionManager::Get().CloseSession(SessionId);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(Result, TEXT("Session closed"));
}

// ============================================================================
// ClaireonAnimGraphTool_Save
// ============================================================================

FString ClaireonAnimGraphTool_Save::GetOperation() const { return TEXT("save"); }

FString ClaireonAnimGraphTool_Save::GetDescription() const
{
    return TEXT("Compile and save the Animation Blueprint to disk for the active anim_graph session. Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	if (!AnimBP)
	{
		return MakeErrorResult(TEXT("AnimBP no longer valid"));
	}

	// Compile first
	FKismetEditorUtilities::CompileBlueprint(AnimBP, EBlueprintCompileOptions::SkipGarbageCollection);

	// Save — use direct UPackage::SavePackage for AnimBPs (ClaireonAssetUtils::SaveAsset
	// uses the CDO lookup path which fails for newly created AnimBPs)
	UPackage* Package = AnimBP->GetOutermost();
	FString PackageFileName;
	if (!FPackageName::DoesPackageExist(Package->GetName(), &PackageFileName))
	{
		// New package — compute the filename from the package name
		PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	}

	Package->SetIsExternallyReferenceable(true);
	Package->MarkPackageDirty();

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	bool bSaveOk = UPackage::SavePackage(Package, AnimBP, *PackageFileName, SaveArgs);
	if (!bSaveOk)
	{
		return MakeErrorResult(FString::Printf(TEXT("Save failed for package: %s"), *Package->GetName()));
	}

	// No explicit refresh — compile already notifies the editor via standard mechanisms

	Data->Cursor.LastOperationStatus = TEXT("Saved");

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetStringField(TEXT("status"), TEXT("saved"));
	return MakeSuccessResult(Result, FString::Printf(TEXT("Saved %s"), *AnimBP->GetPathName()));
}

// ============================================================================
// ClaireonAnimGraphTool_Compile
// ============================================================================

FString ClaireonAnimGraphTool_Compile::GetOperation() const { return TEXT("compile"); }

FString ClaireonAnimGraphTool_Compile::GetDescription() const
{
    return TEXT("Compile the Animation Blueprint of the current session and report any warnings or errors. Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_Compile::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_Compile::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	if (!AnimBP)
	{
		return MakeErrorResult(TEXT("AnimBP no longer valid"));
	}

	FKismetEditorUtilities::CompileBlueprint(AnimBP);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("session_id"), SessionId);
	Result->SetStringField(TEXT("status"), AnimBP->Status == EBlueprintStatus::BS_UpToDate ? TEXT("success") : TEXT("has_issues"));

	// Collect warnings/errors via existing helper
	TSharedPtr<FJsonObject> Warnings = ClaireonAnimGraphHelpers::CollectWarnings(AnimBP);
	if (Warnings)
	{
		Result->SetObjectField(TEXT("warnings"), Warnings);
	}

	Data->Cursor.LastOperationStatus = TEXT("Compiled");
	return MakeSuccessResult(Result, TEXT("Compiled Animation Blueprint"));
}

// ============================================================================
// ClaireonAnimGraphTool_SwitchGraph
// ============================================================================

FString ClaireonAnimGraphTool_SwitchGraph::GetOperation() const { return TEXT("switch_graph"); }

FString ClaireonAnimGraphTool_SwitchGraph::GetDescription() const
{
    return TEXT("Switch the active graph within the current Animation Blueprint session, then return the new active-graph state. Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_SwitchGraph::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("graph_name"), TEXT("Name of the graph to switch to (use with parent_node_guid to disambiguate duplicate names)"));
	S.AddString(TEXT("parent_node_guid"), TEXT("GUID of the parent node whose sub-graph/BoundGraph to navigate to (disambiguates duplicate graph names)"));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_SwitchGraph::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UEdGraph* NewGraph = nullptr;

	// If parent_node_guid is provided, navigate to that node's sub-graph (BoundGraph)
	// This disambiguates graphs with duplicate names (e.g., multiple "Transition" graphs)
	FString ParentNodeGuid;
	if (Arguments->TryGetStringField(TEXT("parent_node_guid"), ParentNodeGuid) && !ParentNodeGuid.IsEmpty())
	{
		FGuid ParsedGuid;
		if (!FGuid::Parse(ParentNodeGuid, ParsedGuid))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid parent_node_guid: %s"), *ParentNodeGuid));
		}

		// Search all graphs for this node
		TArray<ClaireonAnimGraphHelpers::FAnimGraphInfo> AllGraphs = ClaireonAnimGraphHelpers::CollectAllGraphs(Data->AnimBlueprint.Get());
		for (const auto& GraphInfo : AllGraphs)
		{
			if (!GraphInfo.Graph) continue;
			for (UEdGraphNode* Node : GraphInfo.Graph->Nodes)
			{
				if (Node && Node->NodeGuid == ParsedGuid)
				{
					// Found the parent node — get its sub-graphs
					TArray<UEdGraph*> SubGraphs = Node->GetSubGraphs();
					if (SubGraphs.Num() > 0)
					{
						NewGraph = SubGraphs[0];
					}
					else
					{
						// Try BoundGraph via reflection (for state/transition nodes)
						FObjectProperty* BoundGraphProp = CastField<FObjectProperty>(
							Node->GetClass()->FindPropertyByName(TEXT("BoundGraph")));
						if (BoundGraphProp)
						{
							NewGraph = Cast<UEdGraph>(BoundGraphProp->GetObjectPropertyValue_InContainer(Node));
						}
					}
					if (!NewGraph)
					{
						return MakeErrorResult(FString::Printf(TEXT("Node %s has no sub-graph"), *ParentNodeGuid));
					}
					break;
				}
			}
			if (NewGraph) break;
		}

		if (!NewGraph)
		{
			return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s"), *ParentNodeGuid));
		}
	}
	else
	{
		FString GraphName;
		if (!Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
		{
			return MakeErrorResult(TEXT("Missing required field: graph_name or parent_node_guid"));
		}

		FString GraphError;
		NewGraph = ClaireonAnimGraphHelpers::FindAnimGraphByName(Data->AnimBlueprint.Get(), GraphName, GraphError);
		if (!NewGraph)
		{
			return MakeErrorResult(GraphError);
		}
	}

	Data->CurrentGraph = NewGraph;
	Data->Cursor.GraphName = NewGraph->GetName();
	Data->Cursor.FocusedNodeGuid = FGuid();
	Data->Cursor.FocusedPinName = NAME_None;
	Data->Cursor.CursorHistory.Empty();

	// Focus on first relevant node
	for (UEdGraphNode* Node : NewGraph->Nodes)
	{
		if (!Node) continue;
		FString Category = ClaireonAnimGraphHelpers::GetAnimNodeCategory(Node);
		if (Category == TEXT("output_pose") || Category == TEXT("state_entry"))
		{
			Data->Cursor.FocusedNodeGuid = Node->NodeGuid;
			break;
		}
	}

	Data->ResponseMode = TEXT("full"); // Always return full on graph switch
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Switched to graph: %s (%d nodes)"),
		*NewGraph->GetName(), NewGraph->Nodes.Num());

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_GetState
// ============================================================================

FString ClaireonAnimGraphTool_GetState::GetOperation() const { return TEXT("get_state"); }

FString ClaireonAnimGraphTool_GetState::GetDescription() const
{
    return TEXT("Get the full current state of the animation graph editing session (active graph, node count, focus). Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_GetState::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_GetState::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	Data->ResponseMode = TEXT("full");
	Data->Cursor.LastOperationStatus = TEXT("State requested");
	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
