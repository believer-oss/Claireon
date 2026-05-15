// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_SetPinValue.h"
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


FString ClaireonBlueprintGraphTool_SetPinValue::GetName() const
{
    return TEXT("claireon.blueprint_graph_set_pin_value");
}

FString ClaireonBlueprintGraphTool_SetPinValue::GetDescription() const
{
    return TEXT("Set a pin's default literal value (input pin, no connection).");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SetPinValue::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("node_guid"), TEXT("GUID of the node that owns the pin."), true);
    Builder.AddString(TEXT("pin_name"), TEXT("Pin name on that node."), true);
    Builder.AddString(TEXT("value"), TEXT("Default value, encoded as the pin's literal string form."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SetPinValue::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("set_pin_value"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return CheckMutationAffectedNodes(TEXT("set_pin_value"), Data, Operation_SetPinValue(SessionId, Data, Params));
}

FToolResult ClaireonBlueprintGraphEditToolBase::Operation_SetPinValue(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	// Get node and pin
	FString NodeGuidStr, PinName, Value;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}
	if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
	{
		return MakeErrorResult(TEXT("Missing required field: pin_name"));
	}
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required field: value"));
	}

	// Parse GUID
	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));
	}

	// Find node
	UEdGraphNode* Node = ClaireonBPGraphInternal::FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	// Resolve pin using fuzzy matching
	TArray<FString> ResolutionWarnings;
	ClaireonNameResolver::FNameResolveResult SetPinResult;
	UEdGraphPin* Pin = ClaireonNameResolver::ResolvePinName(Node, PinName, EGPD_MAX, SetPinResult);
	if (!Pin)
	{
		return MakeErrorResult(SetPinResult.Error);
	}
	if (!SetPinResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(SetPinResult.ResolutionNote);
	}

	// Validate pin can have default value (must be input pin with no connection)
	if (Pin->Direction != EGPD_Input)
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot set default value on output pin: %s"), *PinName));
	}

	if (Pin->LinkedTo.Num() > 0)
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot set default value on connected pin: %s"), *PinName));
	}

	// Set the default value using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blueprint Pin Value")));
	Blueprint->Modify();
	Graph->Modify();
	Node->Modify();

	// Get schema for validation
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	K2Schema->TrySetDefaultValue(*Pin, Value);

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Set [%s].%s = '%s'"),
		*NodeTitle, *PinName, *Value);

	// Populate affected nodes: the node whose pin value changed
	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);

	FToolResult SetPinValueResult = BuildStateResponse(SessionId, Data);
	SetPinValueResult.Warnings.Append(ResolutionWarnings);
	return SetPinValueResult;
}

#undef LOCTEXT_NAMESPACE
