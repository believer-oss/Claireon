// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_SetRootComponent.h"
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


FString ClaireonBlueprintGraphTool_SetRootComponent::GetOperation() const { return TEXT("graph_set_root_component"); }

FString ClaireonBlueprintGraphTool_SetRootComponent::GetDescription() const
{
    return TEXT("Designate a scene component as the new root of the Blueprint's Simple Construction Script in the open editing session. Requires open session_id from blueprint_graph_open (or pass asset_path to auto-open). Transactional. Common pitfall: only USceneComponent-derived components can be roots; non-scene components error.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SetRootComponent::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("component_name"), TEXT("Name of the scene component to promote to root."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SetRootComponent::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("set_root_component"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return MakeErrorResult(TEXT("Blueprint does not have a SimpleConstructionScript (not an Actor Blueprint?)"));
	}

	// Parse component_name
	FString ComponentName;
	if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return MakeErrorResult(TEXT("Missing required field: component_name"));
	}

	// Find target node
	USCS_Node* TargetNode = SCS->FindSCSNode(FName(*ComponentName));
	if (!TargetNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
	}

	// Verify not inherited
	if (!SCS->GetAllNodes().Contains(TargetNode))
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot set inherited component as root: %s"), *ComponentName));
	}

	// Verify target is a USceneComponent
	if (!TargetNode->ComponentTemplate || !TargetNode->ComponentTemplate->IsA<USceneComponent>())
	{
		return MakeErrorResult(FString::Printf(TEXT("Component '%s' is not a scene component and cannot be the root"), *ComponentName));
	}

	// Check if target is already the scene root
	USCS_Node* CurrentRootNode = nullptr;
	SCS->GetSceneRootComponentTemplate(false, &CurrentRootNode);
	if (CurrentRootNode == TargetNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Component '%s' is already the scene root"), *ComponentName));
	}

	// Get the current scene root (including DefaultSceneRootNode if applicable)
	USCS_Node* OldRootNode = nullptr;
	SCS->GetSceneRootComponentTemplate(true, &OldRootNode);
	bool bWasDefaultSceneRoot = (OldRootNode == SCS->GetDefaultSceneRootNode());

	// Perform root swap with undo support
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Root Component")));
	SCS->Modify();

	// Step 1: Detach target from current parent (if it has one)
	USCS_Node* TargetParent = SCS->FindParentNode(TargetNode);
	if (TargetParent)
	{
		TargetParent->RemoveChildNode(TargetNode);
	}

	// Reset target's relative transform since it becomes root
	if (USceneComponent* SceneComp = Cast<USceneComponent>(TargetNode->ComponentTemplate))
	{
		SceneComp->SetRelativeLocation(FVector::ZeroVector);
		SceneComp->SetRelativeRotation(FRotator::ZeroRotator);
		// Preserve scale intentionally -- scale is often set at authoring time
	}

	// Step 2: Remove old root from root node set
	// Suppress ValidateSceneRootNodes until we finish the swap.
	// The transient state (old root removed, new root not yet added) is safe because
	// ValidateSceneRootNodes will be called by AddNode in the next step, at which point
	// the new root is already in the root set. (Review item M1)
	if (OldRootNode)
	{
		SCS->RemoveNode(OldRootNode, /*bValidateSceneRootNodes=*/false);
	}

	// Step 3: Add target as new root
	// AddNode adds to RootNodes and calls ValidateSceneRootNodes, which will
	// auto-remove DefaultSceneRootNode since a real scene component now exists
	SCS->AddNode(TargetNode);

	// Step 4: Reparent old root under new root
	if (!bWasDefaultSceneRoot && OldRootNode)
	{
		// Old root was a real component -- make it a child of the new root
		TargetNode->AddChildNode(OldRootNode);
		OldRootNode->SetParent(TargetNode);
	}
	// If bWasDefaultSceneRoot, the old default root is cleaned up by ValidateSceneRootNodes
	// in step 3's AddNode call, so no reparenting is needed.

	// MarkBlueprintAsStructurallyModified must be the last call after all hierarchy manipulation (Review item L2)
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Set '%s' as the new root component"), *ComponentName);
	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
