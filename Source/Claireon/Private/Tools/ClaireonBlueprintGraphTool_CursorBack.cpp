// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_CursorBack.h"
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


FString ClaireonBlueprintGraphTool_CursorBack::GetOperation() const { return TEXT("cursor_back"); }

FString ClaireonBlueprintGraphTool_CursorBack::GetDescription() const
{
    return TEXT("Navigate the editing cursor back through its history in the open Blueprint editing session. Requires open session_id from bp_open. Read-only with respect to graph contents (the cursor is session state). Common pitfall: errors when history is empty; use bp_get_state to inspect the current cursor position. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_CursorBack::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_CursorBack::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("cursor_back"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// Pop history, skipping any entries whose graph or node has been deleted
	// while the session was alive. Each iteration tries to resolve both the
	// graph AND the node; if either resolution fails we pop the next entry.
	FGraphCursorHistoryEntry Entry;
	UEdGraph* TargetGraph = nullptr;
	UEdGraphNode* PreviousNode = nullptr;

	while (true)
	{
		if (!Data->Cursor.PopHistory(Entry))
		{
			return MakeErrorResult(TEXT("Cursor history is empty"));
		}

		// Resolve graph for this entry.
		if (Entry.GraphName == Data->Cursor.GraphName)
		{
			TargetGraph = Data->Graph.Get();
		}
		else
		{
			TargetGraph = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, Entry.GraphName);
		}
		if (!TargetGraph)
		{
			// Graph was deleted; try the next entry.
			continue;
		}

		// Resolve node within the target graph.
		PreviousNode = ClaireonBlueprintHelpers::FindNodeByGuid(TargetGraph, Entry.NodeGuid);
		if (!PreviousNode)
		{
			// Node was deleted; try the next entry.
			continue;
		}

		break;
	}

	// Commit a graph switch if the resolved entry lives on a different graph.
	if (Entry.GraphName != Data->Cursor.GraphName)
	{
		Data->Graph = TargetGraph;
		Data->Cursor.GraphName = TargetGraph->GetName();
	}

	// Move cursor to the previous node.
	Data->Cursor.FocusedNodeGuid = Entry.NodeGuid;

	UEdGraphPin* FirstOutputPin = ClaireonBlueprintHelpers::GetFirstOutputPin(PreviousNode);
	if (FirstOutputPin)
	{
		Data->Cursor.FocusedPinName = FirstOutputPin->PinName;
		Data->Cursor.FocusedPinDirection = FirstOutputPin->Direction;
	}
	else
	{
		Data->Cursor.FocusedPinName = NAME_None;
	}

	const FString NodeTitle = PreviousNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Navigated back to: %s (graph: %s)"),
		*NodeTitle, *Data->Cursor.GraphName);

	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
