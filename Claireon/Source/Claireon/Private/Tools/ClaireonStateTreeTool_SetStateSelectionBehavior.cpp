// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_SetStateSelectionBehavior.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_SetStateSelectionBehavior::GetName() const
{
	return TEXT("claireon.statetree_set_state_selection_behavior");
}

FString ClaireonStateTreeTool_SetStateSelectionBehavior::GetDescription() const
{
	return TEXT("Set the selection behavior for a state in the open State Tree editing session. Requires open session_id from claireon.statetree_open. Transactional. Behavior controls whether the state is entered directly, only through its children, or by utility scoring. Common pitfall: changing behavior may force re-entry semantics at runtime.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_SetStateSelectionBehavior::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state."), true);
	Builder.AddString(TEXT("behavior"), TEXT("Selection behavior: None, TryEnterState, TrySelectChildrenInOrder, TrySelectChildrenAtRandom, TrySelectChildrenWithHighestUtility, TrySelectChildrenAtRandomWeightedByUtility, TryFollowTransitions."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_SetStateSelectionBehavior::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FGuid StateId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);

	FString BehaviorStr;
	if (!Arguments->TryGetStringField(TEXT("behavior"), BehaviorStr))
		return MakeErrorResult(TEXT("Missing parameter: behavior"));

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set State Selection Behavior")));
	Data->StateTree->Modify();
	State->SelectionBehavior = ClaireonStateTreeEditInternal::ParseSelectionBehavior(BehaviorStr);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("set_state_selection_behavior -> '%s' set to %s"), *State->Name.ToString(), *BehaviorStr);

	return BuildStateResponse(SessionId, Data);
}
