// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "Dom/JsonObject.h"
#include "Tools/ClaireonSpecApplicator_Blueprint.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_Event.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Timeline.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Select.h"
#include "K2Node_MacroInstance.h"
#include "Engine/MemberReference.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_Literal.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeMap.h"
#include "K2Node_MakeSet.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_AddPinInterface.h"
#include "K2Node_Switch.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_ForEachElementInEnum.h"
#include "K2Node_DoOnceMultiInput.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphUtilities.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ScopedTransaction.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraph.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "K2Node_Tunnel.h"
#include "ClaireonBlueprintNodeSerializer.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonBPInterfaceAuthor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;


FString ClaireonBlueprintGraphTool_Open::GetName() const
{
    return TEXT("claireon.blueprint_graph_open");
}

FString ClaireonBlueprintGraphTool_Open::GetDescription() const
{
    return TEXT("Open an existing Blueprint for editing. Creates or reuses a session keyed by the MCP client, and returns the session id plus initial graph state.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_Open::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path."), true);
    Builder.AddString(TEXT("graph_name"), TEXT("Optional graph to focus initially; defaults to EventGraph."));
    Builder.AddNumber(TEXT("timeout_minutes"), TEXT("Session timeout in minutes (default 60)."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // open is stateless; it manages its own session lifecycle.
    TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* NestedObj = nullptr;
        if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
        {
            Params = *NestedObj;
        }
    }
    return Operation_Open(Params);
}

FToolResult ClaireonBlueprintGraphEditToolBase::Operation_Open(const TSharedPtr<FJsonObject>& Params)
{
	// Register delegate on first use
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonBlueprintGraphEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	// Get asset_path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	// Load Blueprint
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));
	}

	// Get graph_name (optional, defaults to EventGraph)
	FString GraphName;
	if (!Params->TryGetStringField(TEXT("graph_name"), GraphName))
	{
		GraphName = TEXT("EventGraph");
	}

	// Find the graph
	UEdGraph* Graph = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return MakeErrorResult(FString::Printf(TEXT("Graph '%s' not found in Blueprint %s"), *GraphName, *AssetPath));
	}

	// Open session via the manager (handles locking)
	double TimeoutMinutes = 60.0;
	Params->TryGetNumberField(TEXT("timeout_minutes"), TimeoutMinutes);
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(AssetPath, TEXT("claireon.blueprint_edit_graph"), TimeoutMinutes);

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first, or use mcp_release_sessions(asset_path='%s') to force-release it."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60,
			*AssetPath));
	}

	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	}

	const FString& SessionId = OpenResult.SessionId;

	// Create tool-specific data (or reuse if session was reused). The init block
	// is shared with ResolveOrOpenSession's asset_path branch -- see
	// InitToolDataForSession. We re-Find after the helper to handle possible
	// TMap rehash.
	FBlueprintEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		InitToolDataForSession(SessionId, Blueprint, Graph);
		Data = ToolData.Find(SessionId);

		UE_LOG(LogClaireon, Log, TEXT("[EditBlueprintGraph] Created session %s for Blueprint %s"), *SessionId, *Blueprint->GetPathName());
	}

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Opened Blueprint %s, Graph %s"), *AssetPath, *GraphName);

	// "open" always returns the full graph regardless of response_mode — gives initial orientation
	Data->ResponseMode = TEXT("full");
	Data->bSuppressOutput = false;

	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
