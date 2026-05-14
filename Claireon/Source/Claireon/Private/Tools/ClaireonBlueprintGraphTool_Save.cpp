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


FString ClaireonBlueprintGraphTool_Save::GetName() const
{
    return TEXT("claireon.blueprint_graph_save");
}

TArray<FString> ClaireonBlueprintGraphTool_Save::GetSearchKeywords() const
{
    return {TEXT("bp"), TEXT("save"), TEXT("write"), TEXT("persist"), TEXT("commit"), TEXT("graph")};
}

FString ClaireonBlueprintGraphTool_Save::GetDescription() const
{
    return TEXT("Compiles and saves the Blueprint package to disk for the current session. Per the per-node cycle, call save every 1-3 add_node operations to flush in-session edits. Most-common pitfall: skipping save until close, which loses progress on editor crash and forces a full re-author of the in-session graph.");
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
	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[EditBlueprintGraph] Save: Blueprint is no longer valid"));
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	UPackage* Package = Blueprint->GetOutermost();
	if (!Package)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[EditBlueprintGraph] Save: Failed to get package for Blueprint"));
		return MakeErrorResult(TEXT("Failed to get package for Blueprint"));
	}

	// Compile the Blueprint to ensure it's in a valid state before saving
	// This initializes the generated class and ensures the Blueprint is complete
	UE_LOG(LogClaireon, Log, TEXT("[EditBlueprintGraph] Save: Compiling Blueprint before save"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

	// Ensure package is properly configured for saving
	Package->SetIsExternallyReferenceable(true);
	Package->MarkPackageDirty();

	// Save package
	FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

	UE_LOG(LogClaireon, Log, TEXT("[EditBlueprintGraph] Save: Attempting to save to %s"), *PackageFileName);

	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	}
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_None; // Report errors - we expect save to succeed now

	if (UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs))
	{
		UE_LOG(LogClaireon, Log, TEXT("[EditBlueprintGraph] Save: Successfully saved Blueprint to %s"), *PackageFileName);
		Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Saved Blueprint to %s"), *PackageFileName);
		return BuildStateResponse(SessionId, Data);
	}
	else
	{
		UE_LOG(LogClaireon, Error, TEXT("[EditBlueprintGraph] Save: Failed to save Blueprint to %s"), *PackageFileName);
		return MakeErrorResult(FString::Printf(TEXT("Failed to save Blueprint to %s"), *PackageFileName));
	}
}

// ----------------------------------------------------------------------------
// P1: hot-path metadata enrichment
// ----------------------------------------------------------------------------

FString ClaireonBlueprintGraphTool_Save::GetFullDescription() const
{
    return TEXT(
        "Compiles and saves the Blueprint package to disk for the current "
        "session. As part of the per-node incremental cycle, "
        "call claireon.blueprint_graph_save every 1-3 add_node operations rather "
        "than batching dozens of edits before a single save. This protects "
        "against editor crashes (which would otherwise drop in-session edits) "
        "and ensures git diffs stay scoped per logical authoring unit. The "
        "tool internally calls FKismetEditorUtilities::CompileBlueprint and "
        "then UEditorAssetLibrary::SaveLoadedAsset on the package; if compile "
        "errors are present, save still proceeds (the asset is saved with "
        "compile errors recorded). Use claireon.blueprint_graph_compile to "
        "fetch the structured error list.");
}

FString ClaireonBlueprintGraphTool_Save::GetExampleUsage() const
{
    return TEXT("claireon.blueprint_graph_save session_id=\"...\"");
}

#undef LOCTEXT_NAMESPACE
