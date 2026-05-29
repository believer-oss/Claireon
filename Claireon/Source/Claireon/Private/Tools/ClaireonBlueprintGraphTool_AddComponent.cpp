// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_AddComponent.h"
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


FString ClaireonBlueprintGraphTool_AddComponent::GetOperation() const { return TEXT("add_component"); }

FString ClaireonBlueprintGraphTool_AddComponent::GetDescription() const
{
    return TEXT("Add a component to the Blueprint's Simple Construction Script in the open editing session. Requires open session_id from bp_open (or pass asset_path to auto-open). Transactional. Common pitfall: only Actor-derived Blueprints have an SCS; non-Actor BPs error. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddComponent::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("component_class"), TEXT("Component class path (e.g. /Script/Engine.StaticMeshComponent)."), true);
    Builder.AddString(TEXT("component_name"), TEXT("Name for the new component."), true);
    Builder.AddString(TEXT("parent_name"), TEXT("Optional parent component name; defaults to root."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_AddComponent::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("add_component"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	UBlueprint* Blueprint = Data->Blueprint.Get();

	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// Get component name and class
	FString ComponentName, ComponentClass;
	if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return MakeErrorResult(TEXT("Missing required field: component_name"));
	}
	if (!Params->TryGetStringField(TEXT("component_class"), ComponentClass))
	{
		return MakeErrorResult(TEXT("Missing required field: component_class"));
	}

	// Find component class using fuzzy resolution (handles U prefix, Component suffix, etc.)
	TArray<FString> ResolutionWarnings;
	ClaireonNameResolver::FNameResolveResult CompClassResult;
	UClass* CompClass = ClaireonNameResolver::ResolveClassName(ComponentClass, UActorComponent::StaticClass(), CompClassResult);
	if (!CompClass)
	{
		return MakeErrorResult(CompClassResult.Error);
	}
	if (!CompClassResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(CompClassResult.ResolutionNote);
	}

	if (!CompClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Class '%s' is not a component class"), *ComponentClass));
	}

	// Get or create SimpleConstructionScript
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return MakeErrorResult(TEXT("Blueprint does not have a SimpleConstructionScript (not an Actor Blueprint?)"));
	}

	// Get optional parent component
	FString ParentComponentName;
	USCS_Node* ParentNode = nullptr;
	if (Params->TryGetStringField(TEXT("parent_component"), ParentComponentName))
	{
		// Find parent component node
		ParentNode = SCS->FindSCSNode(FName(*ParentComponentName));

		if (!ParentNode)
		{
			return MakeErrorResult(FString::Printf(TEXT("Parent component not found: %s"), *ParentComponentName));
		}
	}

	// Create component using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Blueprint Component")));
	Blueprint->Modify();
	SCS->Modify();

	// Create new SCS node
	USCS_Node* NewNode = SCS->CreateNode(CompClass, FName(*ComponentName));
	if (!NewNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create component node: %s"), *ComponentName));
	}

	// Add to SCS
	if (ParentNode)
	{
		ParentNode->AddChildNode(NewNode);
	}
	else
	{
		SCS->AddNode(NewNode);
	}

	// Mark Blueprint as structurally modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Added component: %s (%s)"),
		*ComponentName, *ComponentClass);

	FToolResult AddCompResult = BuildStateResponse(SessionId, Data);
	AddCompResult.Warnings.Append(ResolutionWarnings);
	return AddCompResult;
}

#undef LOCTEXT_NAMESPACE
