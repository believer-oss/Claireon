// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_MoveCursor.h"
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


FString ClaireonBlueprintGraphTool_MoveCursor::GetName() const
{
    return TEXT("claireon.blueprint_graph_move_cursor");
}

FString ClaireonBlueprintGraphTool_MoveCursor::GetDescription() const
{
    return TEXT("Move the editing cursor to a specific graph/node/pin.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_MoveCursor::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("node_title"), TEXT("Title of the target node."));
    Builder.AddString(TEXT("node_guid"), TEXT("GUID of the target node (alternative to node_title)."));
    Builder.AddString(TEXT("pin_name"), TEXT("Optional pin name to focus on."));
    Builder.AddString(TEXT("direction"), TEXT("Optional pin direction: 'input' | 'output'."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_MoveCursor::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("move_cursor"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_MoveCursor(SessionId, Data, Params);
}

FToolResult ClaireonBlueprintGraphEditToolBase::Operation_MoveCursor(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UEdGraph* Graph = Data->Graph.Get();

	if (!Graph)
	{
		return MakeErrorResult(TEXT("Graph is no longer valid"));
	}

	// Get direction
	FString Direction;
	if (!Params->TryGetStringField(TEXT("direction"), Direction))
	{
		return MakeErrorResult(TEXT("Missing required field: direction"));
	}

	// Get current cursor node
	if (!Data->Cursor.FocusedNodeGuid.IsValid())
	{
		return MakeErrorResult(TEXT("Cursor is not focused on any node"));
	}

	UEdGraphNode* CurrentNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, Data->Cursor.FocusedNodeGuid);
	if (!CurrentNode)
	{
		return MakeErrorResult(TEXT("Current cursor node not found (may have been deleted)"));
	}

	UEdGraphNode* TargetNode = nullptr;
	FString MovementDescription;

	if (Direction == TEXT("right") || Direction == TEXT("exec_next"))
	{
		// Move to node connected via exec output pin
		TArray<UEdGraphPin*> ExecOutputs = ClaireonBlueprintHelpers::GetExecPins(CurrentNode, false, true);
		if (ExecOutputs.Num() > 0 && ExecOutputs[0]->LinkedTo.Num() > 0)
		{
			TargetNode = ExecOutputs[0]->LinkedTo[0]->GetOwningNode();
			MovementDescription = TEXT("exec flow");
		}
	}
	else if (Direction == TEXT("left") || Direction == TEXT("exec_prev"))
	{
		// Move to node connected via exec input pin
		TArray<UEdGraphPin*> ExecInputs = ClaireonBlueprintHelpers::GetExecPins(CurrentNode, true, false);
		if (ExecInputs.Num() > 0 && ExecInputs[0]->LinkedTo.Num() > 0)
		{
			TargetNode = ExecInputs[0]->LinkedTo[0]->GetOwningNode();
			MovementDescription = TEXT("reverse exec flow");
		}
	}
	else if (Direction == TEXT("up"))
	{
		// Find nearest node above
		float MinDistance = FLT_MAX;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node != CurrentNode && Node->NodePosY < CurrentNode->NodePosY)
			{
				float Distance = FMath::Abs(Node->NodePosX - CurrentNode->NodePosX) + (CurrentNode->NodePosY - Node->NodePosY);
				if (Distance < MinDistance)
				{
					MinDistance = Distance;
					TargetNode = Node;
				}
			}
		}
		MovementDescription = TEXT("spatial up");
	}
	else if (Direction == TEXT("down"))
	{
		// Find nearest node below
		float MinDistance = FLT_MAX;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node != CurrentNode && Node->NodePosY > CurrentNode->NodePosY)
			{
				float Distance = FMath::Abs(Node->NodePosX - CurrentNode->NodePosX) + (Node->NodePosY - CurrentNode->NodePosY);
				if (Distance < MinDistance)
				{
					MinDistance = Distance;
					TargetNode = Node;
				}
			}
		}
		MovementDescription = TEXT("spatial down");
	}
	else if (Direction == TEXT("next_pin"))
	{
		// Move to next pin on current node
		if (Data->Cursor.FocusedPinName != NAME_None)
		{
			bool FoundCurrent = false;
			for (UEdGraphPin* Pin : CurrentNode->Pins)
			{
				if (FoundCurrent && Pin->Direction == EGPD_Output)
				{
					Data->Cursor.FocusedPinName = Pin->PinName;
					Data->Cursor.FocusedPinDirection = Pin->Direction;
					Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Moved to next pin: %s"), *Pin->PinName.ToString());
					return BuildStateResponse(SessionId, Data);
				}
				if (Pin->PinName == Data->Cursor.FocusedPinName)
				{
					FoundCurrent = true;
				}
			}
		}
		return MakeErrorResult(TEXT("No next pin available"));
	}
	else if (Direction == TEXT("prev_pin"))
	{
		// Move to previous pin on current node
		if (Data->Cursor.FocusedPinName != NAME_None)
		{
			UEdGraphPin* PrevPin = nullptr;
			for (UEdGraphPin* Pin : CurrentNode->Pins)
			{
				if (Pin->PinName == Data->Cursor.FocusedPinName && PrevPin)
				{
					Data->Cursor.FocusedPinName = PrevPin->PinName;
					Data->Cursor.FocusedPinDirection = PrevPin->Direction;
					Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Moved to previous pin: %s"), *PrevPin->PinName.ToString());
					return BuildStateResponse(SessionId, Data);
				}
				if (Pin->Direction == EGPD_Output)
				{
					PrevPin = Pin;
				}
			}
		}
		return MakeErrorResult(TEXT("No previous pin available"));
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown direction: %s (valid: right, left, up, down, exec_next, exec_prev, next_pin, prev_pin)"), *Direction));
	}

	if (!TargetNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("No node found in direction: %s"), *Direction));
	}

	// Move cursor to target node
	Data->Cursor.PushHistory(Data->Cursor.GraphName);
	Data->Cursor.FocusedNodeGuid = TargetNode->NodeGuid;

	// Focus on first output pin
	UEdGraphPin* FirstOutputPin = ClaireonBlueprintHelpers::GetFirstOutputPin(TargetNode);
	if (FirstOutputPin)
	{
		Data->Cursor.FocusedPinName = FirstOutputPin->PinName;
		Data->Cursor.FocusedPinDirection = FirstOutputPin->Direction;
	}
	else
	{
		Data->Cursor.FocusedPinName = NAME_None;
	}

	FString TargetNodeTitle = TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Moved cursor %s to: %s"),
		*MovementDescription, *TargetNodeTitle);

	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
