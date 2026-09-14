// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_SwitchGraph.h"
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
#include "BlueprintEditor.h"
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

namespace ClaireonBPSwitchGraphInternal
{
    // Prefix helpers to avoid unity-build collisions.

    // Observe live editor focus separately from the session cursor.
    struct FEditorFocusReport
    {
        // Post-request focus: <unbound> without a live editor, <none> without a focused graph.
        FString FocusedGraphName = TEXT("<unbound>");

        // Whether this call requested editor focus.
        bool bFocusChangeRequested = false;

        // Describe the observed focus and request outcome consistently on both switch paths.
        FString DescribeAgainst(const UEdGraph* TargetGraph) const
        {
            if (!bFocusChangeRequested)
            {
                return TEXT("no live editor bound to confirm focus");
            }
            if (IsValid(TargetGraph) && FocusedGraphName == TargetGraph->GetName())
            {
                return TEXT("editor confirms focus there");
            }
            return FString::Printf(TEXT("editor is still focused on %s after the request"), *FocusedGraphName);
        }
    };

    // Request focus only through a live binding; otherwise the helper could open a replacement editor.
    // Report immediate observed focus without pumping the engine loop.
    FEditorFocusReport RequestEditorFocus(FBlueprintEditToolData* Data, UEdGraph* TargetGraph)
    {
        FEditorFocusReport Report;
        if (!Data || !IsValid(TargetGraph))
        {
            return Report;
        }

        const EClaireonEditorBindingStatus Status = Data->EditorBinding.Revalidate(TargetGraph);
        if (Status == EClaireonEditorBindingStatus::NotBound
            || Status == EClaireonEditorBindingStatus::BoundEditorClosed)
        {
            return Report;
        }

        FKismetEditorUtilities::BringKismetToFocusAttentionOnObject(TargetGraph);
        Report.bFocusChangeRequested = true;

        if (TSharedPtr<FBlueprintEditor> PinnedEditor = Data->EditorBinding.Editor.Pin())
        {
            UEdGraph* ActuallyFocused = PinnedEditor->GetFocusedGraph();
            Report.FocusedGraphName = IsValid(ActuallyFocused) ? ActuallyFocused->GetName() : TEXT("<none>");
        }

        return Report;
    }
}


FString ClaireonBlueprintGraphTool_SwitchGraph::GetOperation() const { return TEXT("switch_graph"); }

FString ClaireonBlueprintGraphTool_SwitchGraph::GetDescription() const
{
    return TEXT("Switch the open Blueprint editing session to a different graph on the same Blueprint. Requires open session_id from bp_open. Read-only with respect to graph contents (the cursor and graph pointer are session state). Cursor history is preserved across the switch. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SwitchGraph::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("graph_name"), TEXT("Name of the target graph (ubergraph, function, macro, or delegate signature)."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SwitchGraph::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("switch_graph"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	if (!Data->Blueprint.IsValid())
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	FString GraphName;
	if (!Params->TryGetStringField(TEXT("graph_name"), GraphName))
	{
		return MakeErrorResult(TEXT("Missing required field: graph_name"));
	}

	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* NewGraph = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, GraphName);

	if (!IsValid(NewGraph))
	{
		const FString AvailableList = BuildAvailableGraphsList(Blueprint);
		return MakeErrorResult(FString::Printf(
			TEXT("Graph '%s' not found in Blueprint. Available: %s"),
			*GraphName, *AvailableList));
	}

	if (NewGraph == Data->Graph.Get())
	{
		// Read editor focus even when the session is already on the target graph.
		const ClaireonBPSwitchGraphInternal::FEditorFocusReport FocusReport =
			ClaireonBPSwitchGraphInternal::RequestEditorFocus(Data, NewGraph);

		Data->Cursor.LastOperationStatus = FString::Printf(
			TEXT("Already on graph %s (%s)"), *GraphName, *FocusReport.DescribeAgainst(NewGraph));

		FToolResult Result = BuildStateResponse(SessionId, Data);
		if (Result.Data.IsValid())
		{
			Result.Data->SetStringField(TEXT("editor_focused_graph"), FocusReport.FocusedGraphName);
			Result.Data->SetBoolField(TEXT("focus_change_requested"), FocusReport.bFocusChangeRequested);
		}
		return Result;
	}

	Data->Cursor.PushHistory(Data->Cursor.GraphName);

	Data->Graph = NewGraph;
	Data->Cursor.GraphName = NewGraph->GetName();

	UEdGraphNode* EntryNode = ClaireonBPGraphInternal::SelectEntryNodeForSwitch(Blueprint, NewGraph);
	if (IsValid(EntryNode))
	{
		Data->Cursor.FocusedNodeGuid = EntryNode->NodeGuid;
		UEdGraphPin* OutputPin = ClaireonBlueprintHelpers::GetFirstOutputPin(EntryNode);
		if (OutputPin)
		{
			Data->Cursor.FocusedPinName = OutputPin->PinName;
			Data->Cursor.FocusedPinDirection = EGPD_Output;
		}
		else
		{
			Data->Cursor.FocusedPinName = NAME_None;
		}
	}
	else
	{
		Data->Cursor.FocusedNodeGuid = FGuid();
		Data->Cursor.FocusedPinName = NAME_None;
	}

	// Report whether the live editor followed the session switch.
	const ClaireonBPSwitchGraphInternal::FEditorFocusReport FocusReport =
		ClaireonBPSwitchGraphInternal::RequestEditorFocus(Data, NewGraph);

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Switched to graph %s (%d nodes) (%s)"),
		*GraphName, NewGraph->Nodes.Num(), *FocusReport.DescribeAgainst(NewGraph));

	FToolResult Result = BuildStateResponse(SessionId, Data);
	if (Result.Data.IsValid())
	{
		Result.Data->SetStringField(TEXT("editor_focused_graph"), FocusReport.FocusedGraphName);
		Result.Data->SetBoolField(TEXT("focus_change_requested"), FocusReport.bFocusChangeRequested);
	}
	return Result;
}

#undef LOCTEXT_NAMESPACE
