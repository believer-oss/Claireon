// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_RemoveTransition.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_RemoveTransition::GetName() const
{
	return TEXT("claireon.statetree_remove_transition");
}

FString ClaireonStateTreeTool_RemoveTransition::GetDescription() const
{
	return TEXT("Remove a transition from a state in the open State Tree editing session. Requires open session_id from claireon.statetree_open. Transactional. All conditions attached to the transition are deleted with it. The state remains; only the routing edge is removed.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_RemoveTransition::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state."), true);
	Builder.AddString(TEXT("transition_id"), TEXT("GUID of the transition to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_RemoveTransition::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FGuid StateId, TransitionId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("transition_id"), TransitionId, Error))
		return MakeErrorResult(Error);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	int32 Index = State->Transitions.IndexOfByPredicate([&TransitionId](const FStateTreeTransition& T)
	{
		return T.ID == TransitionId;
	});
	if (Index == INDEX_NONE)
		return MakeErrorResult(TEXT("Transition not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Transition")));
	Data->StateTree->Modify();
	State->Transitions.RemoveAt(Index);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = TEXT("remove_transition -> Removed transition");
	return BuildStateResponse(SessionId, Data);
}
