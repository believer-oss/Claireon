// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_RemoveTransitionCondition.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_RemoveTransitionCondition::GetName() const
{
	return TEXT("claireon.statetree_remove_transition_condition");
}

FString ClaireonStateTreeTool_RemoveTransitionCondition::GetDescription() const
{
	return TEXT("Remove a condition node from a transition in the open State Tree editing session. Requires open session_id from claireon.statetree_open. Transactional. Bindings targeting the removed condition's properties are dropped. Transition behavior may relax (firing more often) since one fewer gate must pass.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_RemoveTransitionCondition::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state."), true);
	Builder.AddString(TEXT("transition_id"), TEXT("GUID of the transition."), true);
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the condition node to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_RemoveTransitionCondition::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FStateTreeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	UStateTreeEditorData* EditorData = ClaireonStateTreeEditInternal::GetEditorDataFromSession(Data, Error);
	if (!EditorData)
		return MakeErrorResult(Error);

	FGuid StateId, TransitionId, NodeId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("transition_id"), TransitionId, Error))
		return MakeErrorResult(Error);
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("node_id"), NodeId, Error))
		return MakeErrorResult(Error);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FStateTreeTransition* Trans = ClaireonStateTreeHelpers::FindTransitionById(State, TransitionId);
	if (!Trans)
		return MakeErrorResult(TEXT("Transition not found"));

	int32 Index = Trans->Conditions.IndexOfByPredicate([&NodeId](const FStateTreeEditorNode& N)
	{
		return N.ID == NodeId;
	});
	if (Index == INDEX_NONE)
		return MakeErrorResult(TEXT("Condition not found in transition"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Transition Condition")));
	Data->StateTree->Modify();
	Trans->Conditions.RemoveAt(Index);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = TEXT("remove_transition_condition -> Removed condition");
	return BuildStateResponse(SessionId, Data);
}
