// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_RenameComponent.h"
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


FString ClaireonBlueprintGraphTool_RenameComponent::GetName() const
{
    return TEXT("claireon.blueprint_graph_rename_component");
}

FString ClaireonBlueprintGraphTool_RenameComponent::GetDescription() const
{
    return TEXT("Rename a component variable in the Blueprint's Simple Construction Script in the open editing session. Requires open session_id from claireon.blueprint_graph_open (or pass asset_path to auto-open). Transactional. The new name must be unique within the SCS; references in graph nodes are auto-fixed when possible.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_RenameComponent::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("component_name"), TEXT("Current name of the component."), true);
    Builder.AddString(TEXT("new_name"), TEXT("New name (must be a valid C++ identifier)."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_RenameComponent::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("rename_component"), Params, SessionId, Data, Error))
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

	// Parse component_name and new_name
	FString ComponentName;
	if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return MakeErrorResult(TEXT("Missing required field: component_name"));
	}

	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return MakeErrorResult(TEXT("Missing required field: new_name"));
	}

	// Find node
	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
	}

	// Verify not inherited
	if (!SCS->GetAllNodes().Contains(Node))
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot rename inherited component: %s"), *ComponentName));
	}

	// Validate new name is a valid C++ identifier
	if (NewName.Len() == 0)
	{
		return MakeErrorResult(TEXT("New name cannot be empty"));
	}
	if (FChar::IsDigit(NewName[0]))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid name '%s': cannot start with a digit"), *NewName));
	}
	for (TCHAR Ch : NewName)
	{
		if (!FChar::IsAlnum(Ch) && Ch != TEXT('_'))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid name '%s': contains invalid character '%c'. Only alphanumeric characters and underscores are allowed."), *NewName, Ch));
		}
	}

	// Pre-check for conflicts in all scopes
	FName NewFName(*NewName);

	// Scope 1: SCS component variables
	for (USCS_Node* ExistingNode : SCS->GetAllNodes())
	{
		if (ExistingNode && ExistingNode != Node && ExistingNode->GetVariableName() == NewFName)
		{
			return MakeErrorResult(FString::Printf(TEXT("Name '%s' conflicts with existing SCS component variable"), *NewName));
		}
	}

	// Scope 2: Blueprint-level variables
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == NewFName)
		{
			return MakeErrorResult(FString::Printf(TEXT("Name '%s' conflicts with existing Blueprint variable"), *NewName));
		}
	}

	// Scope 3: Function graph names
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == NewFName)
		{
			return MakeErrorResult(FString::Printf(TEXT("Name '%s' conflicts with existing function name"), *NewName));
		}
	}

	// Perform rename
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Rename Blueprint Component")));

	// RenameComponentMemberVariable handles:
	// - Variable name update on the SCS node
	// - Reference replacement in Blueprint graphs via ReplaceVariableReferences
	// - Child Blueprint validation via ValidateBlueprintChildVariables
	// - Inheritable component handler refresh
	// - Structural modification marking
	FBlueprintEditorUtils::RenameComponentMemberVariable(Blueprint, Node, NewFName);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Renamed component '%s' to '%s'"), *ComponentName, *NewName);
	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
