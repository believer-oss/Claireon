// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_RemovePin.h"
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


FString ClaireonBlueprintGraphTool_RemovePin::GetOperation() const { return TEXT("remove_pin"); }

FString ClaireonBlueprintGraphTool_RemovePin::GetDescription() const
{
    return TEXT("Remove a dynamic pin from a node (Sequence/MakeArray/Switch/etc.) in the open Blueprint editing session. Requires open session_id from blueprint_graph_open (or pass asset_path to auto-open). Transactional. Connections to the removed pin are dropped; non-dynamic pins error out unmodified. Session-mode tool: open via blueprint_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_RemovePin::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("node_guid"), TEXT("GUID of the node that owns the pin."), true);
    Builder.AddString(TEXT("pin_name"), TEXT("Name of the pin to remove (alternative to pin_index)."), false);
    Builder.AddInteger(TEXT("pin_index"), TEXT("Index into the node's dynamic/removable pins (alternative to pin_name)."), false);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_RemovePin::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("remove_pin"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return CheckMutationAffectedNodes(TEXT("remove_pin"), Data, RemovePin_Impl(SessionId, Data, Params));
}

FToolResult ClaireonBlueprintGraphTool_RemovePin::RemovePin_Impl(
    const FString& SessionId,
    FBlueprintEditToolData* Data,
    const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();
	if (!Blueprint || !Graph)
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));

	FString NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));

	UEdGraphNode* Node = ClaireonBPGraphInternal::FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s.\n%s"), *NodeGuidStr, *AvailableNodes));
	}

	// Find the pin to remove -- by name or by index
	FString PinName;
	UEdGraphPin* TargetPin = nullptr;

	TArray<FString> ResolutionWarnings;
	if (Params->TryGetStringField(TEXT("pin_name"), PinName))
	{
		ClaireonNameResolver::FNameResolveResult RemovePinResult;
		TargetPin = ClaireonNameResolver::ResolvePinName(Node, PinName, EGPD_MAX, RemovePinResult);
		if (!TargetPin)
		{
			return MakeErrorResult(RemovePinResult.Error);
		}
		if (!RemovePinResult.ResolutionNote.IsEmpty())
		{
			ResolutionWarnings.Add(RemovePinResult.ResolutionNote);
		}
	}
	else
	{
		int32 PinIndex = -1;
		if (!Params->TryGetNumberField(TEXT("pin_index"), PinIndex))
		{
			return MakeErrorResult(TEXT("Missing required field: pin_name or pin_index"));
		}

		// Collect dynamic/user-added pins (output exec pins for Sequence/Switch, input data pins for containers)
		TArray<UEdGraphPin*> DynamicPins;
		IK2Node_AddPinInterface* AddPinIface = Cast<IK2Node_AddPinInterface>(Node);
		UK2Node_Switch* SwitchNode = Cast<UK2Node_Switch>(Node);

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->ParentPin != nullptr)
				continue;

			if (AddPinIface && AddPinIface->CanRemovePin(Pin))
			{
				DynamicPins.Add(Pin);
			}
			else if (SwitchNode)
			{
				// For switch nodes, dynamic pins are output exec pins that aren't the default pin
				UEdGraphPin* DefaultPin = SwitchNode->GetDefaultPin();
				if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin != DefaultPin)
				{
					DynamicPins.Add(Pin);
				}
			}
		}

		if (PinIndex < 0 || PinIndex >= DynamicPins.Num())
		{
			return MakeErrorResult(FString::Printf(TEXT("pin_index %d out of range. Node has %d removable pins."), PinIndex, DynamicPins.Num()));
		}
		TargetPin = DynamicPins[PinIndex];
	}

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	FString RemovedPinName = TargetPin->PinName.ToString();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Blueprint Pin")));
	Blueprint->Modify();

	// Dispatch removal
	IK2Node_AddPinInterface* AddPinIface = Cast<IK2Node_AddPinInterface>(Node);
	if (AddPinIface)
	{
		if (!AddPinIface->CanRemovePin(TargetPin))
		{
			return MakeErrorResult(FString::Printf(TEXT("Cannot remove pin '%s' from node '%s'"), *RemovedPinName, *NodeTitle));
		}
		AddPinIface->RemoveInputPin(TargetPin);
	}
	else if (UK2Node_Switch* SwitchNode = Cast<UK2Node_Switch>(Node))
	{
		if (SwitchNode->IsA<UK2Node_SwitchEnum>())
		{
			return MakeErrorResult(TEXT("SwitchEnum pins cannot be removed (fixed to enum entries)"));
		}
		SwitchNode->RemovePinFromSwitchNode(TargetPin);
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Node '%s' does not support removing pins"), *NodeTitle));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Removed pin '%s' from node: %s"), *RemovedPinName, *NodeTitle);
	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
	FToolResult RemovePinFinalResult = BuildStateResponse(SessionId, Data);
	RemovePinFinalResult.Warnings.Append(ResolutionWarnings);
	return RemovePinFinalResult;
}

#undef LOCTEXT_NAMESPACE
