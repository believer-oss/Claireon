// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_Save.h"
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


FString ClaireonBlueprintGraphTool_Save::GetOperation() const { return TEXT("save"); }

TArray<FString> ClaireonBlueprintGraphTool_Save::GetSearchKeywords() const
{
    return {TEXT("bp"), TEXT("save"), TEXT("write"), TEXT("persist"), TEXT("commit"), TEXT("graph")};
}

FString ClaireonBlueprintGraphTool_Save::GetDescription() const
{
    return TEXT("Compile and save the Blueprint package to disk for the current session. Per the per-node cycle, call save every 1-3 add_node operations to flush in-session edits. Most-common pitfall: skipping save until close, which loses progress on editor crash and forces a full re-author of the in-session graph. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_Save::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("save"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	// Scrub + compile + save lives on the shared base so bp_save and bp_close_all
	// cannot drift.
	FString SavedPathOrError;
	TArray<FString> SaveWarnings;
	if (!CompileAndSaveSession(Data, SavedPathOrError, SaveWarnings))
	{
		return MakeErrorResult(SavedPathOrError);
	}

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Saved Blueprint to %s"), *SavedPathOrError);
	FToolResult SaveResult = BuildStateResponse(SessionId, Data);
	SaveResult.Warnings.Append(SaveWarnings);
	return SaveResult;
}

// ----------------------------------------------------------------------------
// hot-path metadata enrichment
// ----------------------------------------------------------------------------

FString ClaireonBlueprintGraphTool_Save::GetFullDescription() const
{
    return TEXT(
        "Compiles and saves the Blueprint package to disk for the current "
        "session. Per the per-node cycle in .claude/areas/blueprint-editing.md, "
        "call bp_save every 1-3 add_node operations rather "
        "than batching dozens of edits before a single save. This protects "
        "against editor crashes (which would otherwise drop in-session edits) "
        "and ensures git diffs stay scoped per logical authoring unit. The "
        "tool internally calls FKismetEditorUtilities::CompileBlueprint and "
        "then UEditorAssetLibrary::SaveLoadedAsset on the package; if compile "
        "errors are present, save still proceeds (the asset is saved with "
        "compile errors recorded). Use bp_compile to "
        "fetch the structured error list.");
}

FString ClaireonBlueprintGraphTool_Save::GetExampleUsage() const
{
    return TEXT("bp_save session_id=\"...\"");
}

#undef LOCTEXT_NAMESPACE
