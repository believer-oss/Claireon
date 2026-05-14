// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_ReparentComponent.h"
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


FString ClaireonBlueprintGraphTool_ReparentComponent::GetOperation() const { return TEXT("graph_reparent_component"); }

FString ClaireonBlueprintGraphTool_ReparentComponent::GetDescription() const
{
    return TEXT("Reparent a component within the Blueprint's Simple Construction Script hierarchy in the open editing session. Requires open session_id from blueprint_graph_open (or pass asset_path to auto-open). Transactional. Common pitfall: cycles are rejected; the new parent must not be the component itself or any of its descendants.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ReparentComponent::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("component_name"), TEXT("Component to reparent."), true);
    Builder.AddString(TEXT("parent_component"), TEXT("New parent component name; omit to move to root."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_ReparentComponent::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("reparent_component"), Params, SessionId, Data, Error))
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

	// Parse component_name (required)
	FString ComponentName;
	if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return MakeErrorResult(TEXT("Missing required field: component_name"));
	}

	// Find source node
	USCS_Node* SourceNode = SCS->FindSCSNode(FName(*ComponentName));
	if (!SourceNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
	}

	// Verify source not inherited
	if (!SCS->GetAllNodes().Contains(SourceNode))
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot reparent inherited component: %s"), *ComponentName));
	}

	// Parse optional parent_component
	FString ParentComponentName;
	bool bMoveToRoot = !Params->TryGetStringField(TEXT("parent_component"), ParentComponentName);
	USCS_Node* TargetNode = nullptr;

	if (!bMoveToRoot)
	{
		TargetNode = SCS->FindSCSNode(FName(*ParentComponentName));
		if (!TargetNode)
		{
			return MakeErrorResult(FString::Printf(TEXT("Target parent component not found: %s"), *ParentComponentName));
		}

		// Circular reparenting check: target must not be self or a descendant of source
		if (TargetNode == SourceNode)
		{
			return MakeErrorResult(TEXT("Cannot reparent a component to itself"));
		}
		// IsChildOf checks if TargetNode is a descendant of SourceNode
		// USCS_Node::IsChildOf(USCS_Node* TestParent) returns true if 'this' is a child of TestParent
		// So we check: is TargetNode a child of SourceNode? -> TargetNode->IsChildOf(SourceNode)
		if (TargetNode->IsChildOf(SourceNode))
		{
			return MakeErrorResult(FString::Printf(TEXT("Cannot reparent '%s' under '%s': would create a circular hierarchy (target is a descendant of source)"), *ComponentName, *ParentComponentName));
		}

		// Non-scene component cannot be attached to a parent
		if (SourceNode->ComponentTemplate && !SourceNode->ComponentTemplate->IsA<USceneComponent>())
		{
			return MakeErrorResult(FString::Printf(TEXT("Cannot attach non-scene component '%s' to a parent. Non-scene components must remain at root level."), *ComponentName));
		}
	}

	// Perform reparent with undo support
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Reparent Blueprint Component")));
	SCS->Modify();

	// Detach from current position
	USCS_Node* CurrentParent = SCS->FindParentNode(SourceNode);
	if (CurrentParent)
	{
		CurrentParent->RemoveChildNode(SourceNode);
	}
	else
	{
		// Source is a root node
		SCS->RemoveNode(SourceNode, /*bValidateSceneRootNodes=*/false);
	}

	// Attach to new position
	if (!bMoveToRoot)
	{
		TargetNode->AddChildNode(SourceNode);
		SourceNode->SetParent(TargetNode);
	}
	else
	{
		// Moving to root level
		// NOTE (v1): When reparenting to root in a Blueprint inheriting from another BP with a root
		// component, RemoveNode above clears ParentComponentOrVariableName. This is intentional for v1
		// and may need attention for inherited-BP scenarios.
		SCS->AddNode(SourceNode);
	}

	SCS->ValidateSceneRootNodes();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	FString StatusMsg = bMoveToRoot
		? FString::Printf(TEXT("Reparented component '%s' to root level"), *ComponentName)
		: FString::Printf(TEXT("Reparented component '%s' under '%s'"), *ComponentName, *ParentComponentName);
	Data->Cursor.LastOperationStatus = StatusMsg;
	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
