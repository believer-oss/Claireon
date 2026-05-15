// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimGraphTools_StateMachine.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Tools/ClaireonAnimGraphEditBase.h"
#include "Tools/ClaireonAnimGraphHelpers.h"

#include "Animation/AnimBlueprint.h"
#include "AnimStateTransitionNode.h"
#include "AnimStateNode.h"
#include "AnimStateEntryNode.h"
#include "AnimStateConduitNode.h"
#include "AnimationStateMachineGraph.h"
#include "AnimationStateMachineSchema.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

#define LOCTEXT_NAMESPACE "ClaireonAnimGraphTools_StateMachine"

// ============================================================================
// ClaireonAnimGraphTool_AddState
// ============================================================================

FString ClaireonAnimGraphTool_AddState::GetName() const
{
	return TEXT("claireon.animgraph_add_state");
}

FString ClaireonAnimGraphTool_AddState::GetDescription() const
{
	return TEXT("Add a new state to the current state machine graph.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_AddState::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("state_name"), TEXT("Name for the new state"), true);
	S.AddObject(TEXT("position"), TEXT("Position {x, y} for the new state node"));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_AddState::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();
	UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(Graph);
	if (!SMGraph)
	{
		return MakeErrorResult(TEXT("Current graph is not a state machine. Use switch_graph to navigate to a state machine first."));
	}

	FString StateName;
	if (!Arguments->TryGetStringField(TEXT("state_name"), StateName))
	{
		return MakeErrorResult(TEXT("Missing required field: state_name"));
	}

	FVector2D Position(200.0f, 0.0f);
	const TSharedPtr<FJsonObject>* PosObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("position"), PosObj) && PosObj && (*PosObj).IsValid())
	{
		(*PosObj)->TryGetNumberField(TEXT("x"), Position.X);
		(*PosObj)->TryGetNumberField(TEXT("y"), Position.Y);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add State")));
	AnimBP->Modify();
	Graph->Modify();

	UAnimStateNode* NewState = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateNode>(
		SMGraph, NewObject<UAnimStateNode>(), Position, false);

	if (!NewState)
	{
		return MakeErrorResult(TEXT("Failed to create state node"));
	}

	// Rename the state by setting its BoundGraph name (which becomes the display name)
	if (NewState->BoundGraph)
	{
		NewState->BoundGraph->Rename(*StateName, nullptr, REN_DoNotDirty);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(NewState->NodeGuid);
	Data->Cursor.PushHistory(Data->Cursor.GraphName);
	Data->Cursor.FocusedNodeGuid = NewState->NodeGuid;
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Added state '%s' (GUID: %s)"),
		*StateName, *NewState->NodeGuid.ToString());

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_RemoveState
// ============================================================================

FString ClaireonAnimGraphTool_RemoveState::GetName() const
{
	return TEXT("claireon.animgraph_remove_state");
}

FString ClaireonAnimGraphTool_RemoveState::GetDescription() const
{
	return TEXT("Remove a state from the current state machine graph.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_RemoveState::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("state_guid"), TEXT("GUID of the state to remove"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_RemoveState::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();
	UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(Graph);
	if (!SMGraph)
	{
		return MakeErrorResult(TEXT("Current graph is not a state machine."));
	}

	FString StateGuidStr;
	if (!Arguments->TryGetStringField(TEXT("state_guid"), StateGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: state_guid"));
	}

	FGuid StateGuid;
	FGuid::Parse(StateGuidStr, StateGuid);

	UEdGraphNode* FoundNode = nullptr;
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		if (Node && Node->NodeGuid == StateGuid)
		{
			FoundNode = Node;
			break;
		}
	}

	if (!FoundNode || !FoundNode->IsA<UAnimStateNodeBase>())
	{
		return MakeErrorResult(FString::Printf(TEXT("State node not found with GUID: %s"), *StateGuidStr));
	}

	FString StateName = FoundNode->GetNodeTitle(ENodeTitleType::ListView).ToString();

	// Track connected transition nodes
	for (UEdGraphPin* Pin : FoundNode->Pins)
	{
		if (!Pin) continue;
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (Linked && Linked->GetOwningNode())
				Data->LastOperationAffectedNodes.Add(Linked->GetOwningNode()->NodeGuid);
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove State")));
	AnimBP->Modify();
	Graph->Modify();

	FoundNode->BreakAllNodeLinks();

	// Remove bound graph if it's a state node
	if (UAnimStateNode* StateNode = Cast<UAnimStateNode>(FoundNode))
	{
		if (StateNode->BoundGraph)
		{
			FBlueprintEditorUtils::RemoveGraph(AnimBP, StateNode->BoundGraph);
		}
	}

	SMGraph->RemoveNode(FoundNode);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	Graph->NotifyGraphChanged();

	if (Data->Cursor.FocusedNodeGuid == StateGuid)
	{
		Data->Cursor.FocusedNodeGuid = FGuid();
		Data->Cursor.FocusedPinName = NAME_None;
	}

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Removed state '%s'"), *StateName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_RenameState
// ============================================================================

FString ClaireonAnimGraphTool_RenameState::GetName() const
{
	return TEXT("claireon.animgraph_rename_state");
}

FString ClaireonAnimGraphTool_RenameState::GetDescription() const
{
	return TEXT("Rename a state in the current state machine graph.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_RenameState::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("state_guid"), TEXT("GUID of the state to rename"), true);
	S.AddString(TEXT("new_name"), TEXT("New name for the state"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_RenameState::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();
	UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(Graph);
	if (!SMGraph) return MakeErrorResult(TEXT("Current graph is not a state machine."));

	FString StateGuidStr, NewName;
	if (!Arguments->TryGetStringField(TEXT("state_guid"), StateGuidStr))
		return MakeErrorResult(TEXT("Missing required field: state_guid"));
	if (!Arguments->TryGetStringField(TEXT("new_name"), NewName))
		return MakeErrorResult(TEXT("Missing required field: new_name"));

	FGuid StateGuid;
	FGuid::Parse(StateGuidStr, StateGuid);

	UAnimStateNode* StateNode = nullptr;
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		if (Node && Node->NodeGuid == StateGuid)
		{
			StateNode = Cast<UAnimStateNode>(Node);
			break;
		}
	}
	if (!StateNode) return MakeErrorResult(FString::Printf(TEXT("State not found: %s"), *StateGuidStr));

	FString OldName = StateNode->GetNodeTitle(ENodeTitleType::ListView).ToString();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Rename State")));
	AnimBP->Modify();

	if (StateNode->BoundGraph)
	{
		StateNode->BoundGraph->Rename(*NewName, nullptr, REN_DoNotDirty);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(StateNode->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Renamed state '%s' → '%s'"), *OldName, *NewName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_SetEntryState
// ============================================================================

FString ClaireonAnimGraphTool_SetEntryState::GetName() const
{
	return TEXT("claireon.animgraph_set_entry_state");
}

FString ClaireonAnimGraphTool_SetEntryState::GetDescription() const
{
	return TEXT("Set the entry (default) state in the current state machine graph.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_SetEntryState::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("state_guid"), TEXT("GUID of the state to set as entry"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_SetEntryState::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();
	UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(Graph);
	if (!SMGraph) return MakeErrorResult(TEXT("Current graph is not a state machine."));

	FString StateGuidStr;
	if (!Arguments->TryGetStringField(TEXT("state_guid"), StateGuidStr))
		return MakeErrorResult(TEXT("Missing required field: state_guid"));

	FGuid StateGuid;
	FGuid::Parse(StateGuidStr, StateGuid);

	// Find target state
	UAnimStateNodeBase* TargetState = nullptr;
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		if (Node && Node->NodeGuid == StateGuid)
		{
			TargetState = Cast<UAnimStateNodeBase>(Node);
			break;
		}
	}
	if (!TargetState) return MakeErrorResult(FString::Printf(TEXT("State not found: %s"), *StateGuidStr));

	// Find the entry node
	UAnimStateEntryNode* EntryNode = SMGraph->EntryNode;
	if (!EntryNode)
	{
		for (UEdGraphNode* Node : SMGraph->Nodes)
		{
			if (UAnimStateEntryNode* Entry = Cast<UAnimStateEntryNode>(Node))
			{
				EntryNode = Entry;
				break;
			}
		}
	}
	if (!EntryNode) return MakeErrorResult(TEXT("Entry node not found in state machine"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Entry State")));
	AnimBP->Modify();
	Graph->Modify();

	// Find the entry node's output pin
	UEdGraphPin* EntryPin = nullptr;
	for (UEdGraphPin* Pin : EntryNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output)
		{
			EntryPin = Pin;
			break;
		}
	}

	if (EntryPin)
	{
		EntryPin->BreakAllPinLinks();
	}

	// Connect entry to target state's input pin
	UEdGraphPin* StateInputPin = TargetState->GetInputPin();
	if (EntryPin && StateInputPin)
	{
		const UEdGraphSchema* Schema = Graph->GetSchema();
		Schema->TryCreateConnection(EntryPin, StateInputPin);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	Graph->NotifyGraphChanged();

	FString StateName = TargetState->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->LastOperationAffectedNodes.Add(EntryNode->NodeGuid);
	Data->LastOperationAffectedNodes.Add(TargetState->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Set entry state to '%s'"), *StateName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_AddTransition
// ============================================================================

FString ClaireonAnimGraphTool_AddTransition::GetName() const
{
	return TEXT("claireon.animgraph_add_transition");
}

FString ClaireonAnimGraphTool_AddTransition::GetDescription() const
{
	return TEXT("Add a transition between two states in the current state machine graph.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_AddTransition::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("from_state_guid"), TEXT("GUID of the source state"), true);
	S.AddString(TEXT("to_state_guid"), TEXT("GUID of the target state"), true);
	S.AddObject(TEXT("properties"), TEXT("Optional transition properties to set immediately"));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_AddTransition::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();
	UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(Graph);
	if (!SMGraph) return MakeErrorResult(TEXT("Current graph is not a state machine."));

	FString FromGuidStr, ToGuidStr;
	if (!Arguments->TryGetStringField(TEXT("from_state_guid"), FromGuidStr))
		return MakeErrorResult(TEXT("Missing required field: from_state_guid"));
	if (!Arguments->TryGetStringField(TEXT("to_state_guid"), ToGuidStr))
		return MakeErrorResult(TEXT("Missing required field: to_state_guid"));

	FGuid FromGuid, ToGuid;
	FGuid::Parse(FromGuidStr, FromGuid);
	FGuid::Parse(ToGuidStr, ToGuid);

	UAnimStateNodeBase* FromState = nullptr;
	UAnimStateNodeBase* ToState = nullptr;
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		if (Node && Node->NodeGuid == FromGuid) FromState = Cast<UAnimStateNodeBase>(Node);
		if (Node && Node->NodeGuid == ToGuid) ToState = Cast<UAnimStateNodeBase>(Node);
	}

	if (!FromState) return MakeErrorResult(FString::Printf(TEXT("From state not found: %s"), *FromGuidStr));
	if (!ToState) return MakeErrorResult(FString::Printf(TEXT("To state not found: %s"), *ToGuidStr));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Transition")));
	AnimBP->Modify();
	Graph->Modify();

	// Calculate midpoint position
	FVector2D Location = (FVector2D(FromState->NodePosX, FromState->NodePosY) +
		FVector2D(ToState->NodePosX, ToState->NodePosY)) * 0.5f;

	UAnimStateTransitionNode* TransNode = FEdGraphSchemaAction_NewStateNode::SpawnNodeFromTemplate<UAnimStateTransitionNode>(
		SMGraph, NewObject<UAnimStateTransitionNode>(), Location, false);

	if (!TransNode)
	{
		return MakeErrorResult(TEXT("Failed to create transition node"));
	}

	TransNode->CreateConnections(FromState, ToState);

	// Apply optional properties
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
	{
		const TSharedPtr<FJsonObject>& Props = *PropsObj;

		double Duration;
		if (Props->TryGetNumberField(TEXT("crossfade_duration"), Duration))
			TransNode->CrossfadeDuration = static_cast<float>(Duration);

		int32 Priority;
		if (Props->TryGetNumberField(TEXT("priority_order"), Priority))
			TransNode->PriorityOrder = Priority;

		bool bBidirectional;
		if (Props->TryGetBoolField(TEXT("bidirectional"), bBidirectional))
			TransNode->Bidirectional = bBidirectional;

		bool bAutoRule;
		if (Props->TryGetBoolField(TEXT("automatic_rule_based_on_sequence_player"), bAutoRule))
			TransNode->bAutomaticRuleBasedOnSequencePlayerInState = bAutoRule;

		FString LogicStr;
		if (Props->TryGetStringField(TEXT("logic_type"), LogicStr))
		{
			if (LogicStr == TEXT("StandardBlend")) TransNode->LogicType = ETransitionLogicType::TLT_StandardBlend;
			else if (LogicStr == TEXT("Inertialization")) TransNode->LogicType = ETransitionLogicType::TLT_Inertialization;
			else if (LogicStr == TEXT("Custom")) TransNode->LogicType = ETransitionLogicType::TLT_Custom;
		}

		FString BlendModeStr;
		if (Props->TryGetStringField(TEXT("blend_mode"), BlendModeStr))
		{
			if (BlendModeStr == TEXT("Linear")) TransNode->BlendMode = EAlphaBlendOption::Linear;
			else if (BlendModeStr == TEXT("Cubic")) TransNode->BlendMode = EAlphaBlendOption::Cubic;
			else if (BlendModeStr == TEXT("HermiteCubic")) TransNode->BlendMode = EAlphaBlendOption::HermiteCubic;
			else if (BlendModeStr == TEXT("Sinusoidal")) TransNode->BlendMode = EAlphaBlendOption::Sinusoidal;
			else if (BlendModeStr == TEXT("CubicInOut")) TransNode->BlendMode = EAlphaBlendOption::CubicInOut;
			else if (BlendModeStr == TEXT("Custom")) TransNode->BlendMode = EAlphaBlendOption::Custom;
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	Graph->NotifyGraphChanged();

	FString FromName = FromState->GetNodeTitle(ENodeTitleType::ListView).ToString();
	FString ToName = ToState->GetNodeTitle(ENodeTitleType::ListView).ToString();

	Data->LastOperationAffectedNodes.Add(TransNode->NodeGuid);
	Data->LastOperationAffectedNodes.Add(FromState->NodeGuid);
	Data->LastOperationAffectedNodes.Add(ToState->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Added transition '%s' → '%s' (GUID: %s)"),
		*FromName, *ToName, *TransNode->NodeGuid.ToString());

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_RemoveTransition
// ============================================================================

FString ClaireonAnimGraphTool_RemoveTransition::GetName() const
{
	return TEXT("claireon.animgraph_remove_transition");
}

FString ClaireonAnimGraphTool_RemoveTransition::GetDescription() const
{
	return TEXT("Remove a transition from the current state machine graph.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_RemoveTransition::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("transition_guid"), TEXT("GUID of the transition to remove"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_RemoveTransition::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();
	UAnimationStateMachineGraph* SMGraph = Cast<UAnimationStateMachineGraph>(Graph);
	if (!SMGraph) return MakeErrorResult(TEXT("Current graph is not a state machine."));

	FString TransGuidStr;
	if (!Arguments->TryGetStringField(TEXT("transition_guid"), TransGuidStr))
		return MakeErrorResult(TEXT("Missing required field: transition_guid"));

	FGuid TransGuid;
	FGuid::Parse(TransGuidStr, TransGuid);

	UAnimStateTransitionNode* TransNode = nullptr;
	for (UEdGraphNode* Node : SMGraph->Nodes)
	{
		if (Node && Node->NodeGuid == TransGuid)
		{
			TransNode = Cast<UAnimStateTransitionNode>(Node);
			break;
		}
	}
	if (!TransNode) return MakeErrorResult(FString::Printf(TEXT("Transition not found: %s"), *TransGuidStr));

	FString TransName = TransNode->GetNodeTitle(ENodeTitleType::ListView).ToString();

	// Track connected states
	for (UEdGraphPin* Pin : TransNode->Pins)
	{
		if (!Pin) continue;
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (Linked && Linked->GetOwningNode())
				Data->LastOperationAffectedNodes.Add(Linked->GetOwningNode()->NodeGuid);
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Transition")));
	AnimBP->Modify();
	Graph->Modify();

	TransNode->BreakAllNodeLinks();
	SMGraph->RemoveNode(TransNode);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	Graph->NotifyGraphChanged();

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Removed transition '%s'"), *TransName);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_SetTransitionProperties (IMPLEMENTED)
// ============================================================================

FString ClaireonAnimGraphTool_SetTransitionProperties::GetName() const
{
	return TEXT("claireon.animgraph_set_transition_properties");
}

FString ClaireonAnimGraphTool_SetTransitionProperties::GetDescription() const
{
	return TEXT("Set properties on a state machine transition (crossfade duration, blend mode, logic type, shared rules, etc.).");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_SetTransitionProperties::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("transition_guid"), TEXT("GUID of the transition to modify"), true);
	S.AddObject(TEXT("properties"), TEXT("Properties object: crossfade_duration, priority_order, bidirectional, automatic_rule_based_on_sequence_player, automatic_rule_trigger_time, blend_mode, logic_type, promote_shared, unshare_rules, use_shared_rules, promote_shared_crossfade, unshare_crossfade, use_shared_crossfade"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_SetTransitionProperties::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();
	if (!AnimBP || !Graph)
	{
		return MakeErrorResult(TEXT("AnimBP or Graph no longer valid"));
	}

	// Find the transition node by GUID
	FString TransitionGuidStr;
	if (!Arguments->TryGetStringField(TEXT("transition_guid"), TransitionGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: transition_guid"));
	}

	FGuid TransitionGuid;
	if (!FGuid::Parse(TransitionGuidStr, TransitionGuid))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid GUID format: %s"), *TransitionGuidStr));
	}

	UAnimStateTransitionNode* TransNode = nullptr;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node && Node->NodeGuid == TransitionGuid)
		{
			TransNode = Cast<UAnimStateTransitionNode>(Node);
			break;
		}
	}

	if (!TransNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Transition node not found with GUID: %s"), *TransitionGuidStr));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Transition Properties")));
	TransNode->Modify();

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj || !(*PropsObj).IsValid())
	{
		return MakeErrorResult(TEXT("Missing required field: properties (object)"));
	}
	const TSharedPtr<FJsonObject>& Props = *PropsObj;

	TArray<FString> ChangedProps;

	// Crossfade duration
	double Duration;
	if (Props->TryGetNumberField(TEXT("crossfade_duration"), Duration))
	{
		TransNode->CrossfadeDuration = static_cast<float>(Duration);
		ChangedProps.Add(TEXT("crossfade_duration"));
	}

	// Priority order
	int32 Priority;
	if (Props->TryGetNumberField(TEXT("priority_order"), Priority))
	{
		TransNode->PriorityOrder = Priority;
		ChangedProps.Add(TEXT("priority_order"));
	}

	// Bidirectional
	bool bBidirectional;
	if (Props->TryGetBoolField(TEXT("bidirectional"), bBidirectional))
	{
		TransNode->Bidirectional = bBidirectional;
		ChangedProps.Add(TEXT("bidirectional"));
	}

	// Automatic rule based on sequence player in state
	bool bAutoRule;
	if (Props->TryGetBoolField(TEXT("automatic_rule_based_on_sequence_player"), bAutoRule))
	{
		TransNode->bAutomaticRuleBasedOnSequencePlayerInState = bAutoRule;
		ChangedProps.Add(FString::Printf(TEXT("automatic_rule=%s"), bAutoRule ? TEXT("true") : TEXT("false")));
	}

	// Automatic rule trigger time
	double AutoTriggerTime;
	if (Props->TryGetNumberField(TEXT("automatic_rule_trigger_time"), AutoTriggerTime))
	{
		TransNode->AutomaticRuleTriggerTime = static_cast<float>(AutoTriggerTime);
		ChangedProps.Add(FString::Printf(TEXT("automatic_rule_trigger_time=%.2f"), AutoTriggerTime));
	}

	// Blend mode
	FString BlendModeStr;
	if (Props->TryGetStringField(TEXT("blend_mode"), BlendModeStr))
	{
		if (BlendModeStr == TEXT("Linear")) TransNode->BlendMode = EAlphaBlendOption::Linear;
		else if (BlendModeStr == TEXT("Cubic")) TransNode->BlendMode = EAlphaBlendOption::Cubic;
		else if (BlendModeStr == TEXT("HermiteCubic")) TransNode->BlendMode = EAlphaBlendOption::HermiteCubic;
		else if (BlendModeStr == TEXT("Sinusoidal")) TransNode->BlendMode = EAlphaBlendOption::Sinusoidal;
		else if (BlendModeStr == TEXT("QuadraticInOut")) TransNode->BlendMode = EAlphaBlendOption::QuadraticInOut;
		else if (BlendModeStr == TEXT("CubicInOut")) TransNode->BlendMode = EAlphaBlendOption::CubicInOut;
		else if (BlendModeStr == TEXT("QuarticInOut")) TransNode->BlendMode = EAlphaBlendOption::QuarticInOut;
		else if (BlendModeStr == TEXT("QuinticInOut")) TransNode->BlendMode = EAlphaBlendOption::QuinticInOut;
		else if (BlendModeStr == TEXT("CircularIn")) TransNode->BlendMode = EAlphaBlendOption::CircularIn;
		else if (BlendModeStr == TEXT("CircularOut")) TransNode->BlendMode = EAlphaBlendOption::CircularOut;
		else if (BlendModeStr == TEXT("CircularInOut")) TransNode->BlendMode = EAlphaBlendOption::CircularInOut;
		else if (BlendModeStr == TEXT("ExpIn")) TransNode->BlendMode = EAlphaBlendOption::ExpIn;
		else if (BlendModeStr == TEXT("ExpOut")) TransNode->BlendMode = EAlphaBlendOption::ExpOut;
		else if (BlendModeStr == TEXT("ExpInOut")) TransNode->BlendMode = EAlphaBlendOption::ExpInOut;
		else if (BlendModeStr == TEXT("Custom")) TransNode->BlendMode = EAlphaBlendOption::Custom;
		ChangedProps.Add(TEXT("blend_mode"));
	}

	// Logic type
	FString LogicStr;
	if (Props->TryGetStringField(TEXT("logic_type"), LogicStr))
	{
		if (LogicStr == TEXT("StandardBlend")) TransNode->LogicType = ETransitionLogicType::TLT_StandardBlend;
		else if (LogicStr == TEXT("Inertialization")) TransNode->LogicType = ETransitionLogicType::TLT_Inertialization;
		else if (LogicStr == TEXT("Custom")) TransNode->LogicType = ETransitionLogicType::TLT_Custom;
		ChangedProps.Add(TEXT("logic_type"));
	}

	// Shared rules: promote
	FString PromoteSharedName;
	if (Props->TryGetStringField(TEXT("promote_shared"), PromoteSharedName))
	{
		TransNode->MakeRulesShareable(PromoteSharedName);
		ChangedProps.Add(FString::Printf(TEXT("promote_shared=%s"), *PromoteSharedName));
	}

	// Shared rules: unshare
	bool bUnshareRules;
	if (Props->TryGetBoolField(TEXT("unshare_rules"), bUnshareRules) && bUnshareRules)
	{
		TransNode->UnshareRules();
		ChangedProps.Add(TEXT("unshare_rules"));
	}

	// Shared rules: use shared from another transition
	FString UseSharedGuid;
	if (Props->TryGetStringField(TEXT("use_shared_rules"), UseSharedGuid))
	{
		FGuid SharedGuid;
		if (FGuid::Parse(UseSharedGuid, SharedGuid))
		{
			// Find the source transition node
			UAnimStateTransitionNode* SourceTransNode = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid == SharedGuid)
				{
					SourceTransNode = Cast<UAnimStateTransitionNode>(Node);
					break;
				}
			}
			if (SourceTransNode && SourceTransNode->bSharedRules)
			{
				TransNode->UseSharedRules(SourceTransNode);
				ChangedProps.Add(FString::Printf(TEXT("use_shared_rules=%s"), *SourceTransNode->SharedRulesName));
			}
			else
			{
				return MakeErrorResult(FString::Printf(TEXT("Source transition %s not found or doesn't have shared rules"), *UseSharedGuid));
			}
		}
	}

	// Shared crossfade: promote
	FString PromoteCrossfadeName;
	if (Props->TryGetStringField(TEXT("promote_shared_crossfade"), PromoteCrossfadeName))
	{
		TransNode->MakeCrossfadeShareable(PromoteCrossfadeName);
		ChangedProps.Add(FString::Printf(TEXT("promote_shared_crossfade=%s"), *PromoteCrossfadeName));
	}

	// Shared crossfade: unshare
	bool bUnshareCrossfade;
	if (Props->TryGetBoolField(TEXT("unshare_crossfade"), bUnshareCrossfade) && bUnshareCrossfade)
	{
		TransNode->UnshareCrossade();
		ChangedProps.Add(TEXT("unshare_crossfade"));
	}

	// Shared crossfade: use shared from another transition
	FString UseSharedCrossfadeGuid;
	if (Props->TryGetStringField(TEXT("use_shared_crossfade"), UseSharedCrossfadeGuid))
	{
		FGuid SharedCfGuid;
		if (FGuid::Parse(UseSharedCrossfadeGuid, SharedCfGuid))
		{
			UAnimStateTransitionNode* SourceCfNode = nullptr;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node && Node->NodeGuid == SharedCfGuid)
				{
					SourceCfNode = Cast<UAnimStateTransitionNode>(Node);
					break;
				}
			}
			if (SourceCfNode && SourceCfNode->bSharedCrossfade)
			{
				TransNode->UseSharedCrossfade(SourceCfNode);
				ChangedProps.Add(FString::Printf(TEXT("use_shared_crossfade=%s"), *SourceCfNode->SharedCrossfadeName));
			}
			else
			{
				return MakeErrorResult(FString::Printf(TEXT("Source transition %s not found or doesn't have shared crossfade"), *UseSharedCrossfadeGuid));
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(TransNode->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Set transition properties: %s"), *FString::Join(ChangedProps, TEXT(", ")));

	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
