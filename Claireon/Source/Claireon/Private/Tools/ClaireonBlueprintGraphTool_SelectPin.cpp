// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_SelectPin.h"
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


FString ClaireonBlueprintGraphTool_SelectPin::GetName() const
{
    return TEXT("claireon.blueprint_graph_select_pin");
}

FString ClaireonBlueprintGraphTool_SelectPin::GetDescription() const
{
    return TEXT("Move the cursor to a specific pin on a node.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SelectPin::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("node_guid"), TEXT("GUID of the node that owns the pin."), true);
    Builder.AddString(TEXT("pin_name"), TEXT("Name of the pin to select."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SelectPin::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("select_pin"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_SelectPin(SessionId, Data, Params);
}

FToolResult ClaireonBlueprintGraphEditToolBase::Operation_SelectPin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UEdGraph* Graph = Data->Graph.Get();

	if (!Graph)
	{
		return MakeErrorResult(TEXT("Graph is no longer valid"));
	}

	// Get node_guid and pin_name
	FString NodeGuidStr, PinName;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}
	if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
	{
		return MakeErrorResult(TEXT("Missing required field: pin_name"));
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));
	}

	// Find the node
	UEdGraphNode* Node = ClaireonBPGraphInternal::FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	// Resolve the pin using fuzzy matching
	TArray<FString> ResolutionWarnings;
	ClaireonNameResolver::FNameResolveResult SelectPinResult;
	UEdGraphPin* Pin = ClaireonNameResolver::ResolvePinName(Node, PinName, EGPD_MAX, SelectPinResult);
	if (!Pin)
	{
		return MakeErrorResult(SelectPinResult.Error);
	}
	if (!SelectPinResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(SelectPinResult.ResolutionNote);
	}

	// Move cursor to this pin
	Data->Cursor.PushHistory(Data->Cursor.GraphName);
	Data->Cursor.FocusedNodeGuid = NodeGuid;
	Data->Cursor.FocusedPinName = Pin->PinName;
	Data->Cursor.FocusedPinDirection = Pin->Direction;

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	FString PinDir = (Pin->Direction == EGPD_Input) ? TEXT("input") : TEXT("output");
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Selected pin: [%s].%s (%s)"),
		*NodeTitle, *PinName, *PinDir);

	FToolResult SelectPinFinalResult = BuildStateResponse(SessionId, Data);
	SelectPinFinalResult.Warnings.Append(ResolutionWarnings);
	return SelectPinFinalResult;
}

#undef LOCTEXT_NAMESPACE
