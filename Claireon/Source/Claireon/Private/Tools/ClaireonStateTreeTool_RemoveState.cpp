// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_RemoveState.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_RemoveState::GetName() const
{
	return TEXT("claireon.statetree_remove_state");
}

FString ClaireonStateTreeTool_RemoveState::GetDescription() const
{
	return TEXT("Remove an existing state and its entire subtree from the State Tree in the open editing session. Requires open session_id from claireon.statetree_open. Transactional. Common pitfall: transitions targeting any removed state from elsewhere in the tree are silently broken; verify with claireon.statetree_inspect.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_RemoveState::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_RemoveState::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	{
		return MakeErrorResult(Error);
	}

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
	{
		return MakeErrorResult(FString::Printf(TEXT("State not found: %s"), *StateId.ToString()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove State")));
	Data->StateTree->Modify();

	FString StateName = State->Name.ToString();
	UStateTreeState* ParentState = Cast<UStateTreeState>(State->GetOuter());

	if (ParentState)
	{
		ParentState->Children.Remove(State);
		Data->FocusedStateId = ParentState->ID;
	}
	else
	{
		EditorData->SubTrees.Remove(State);
		Data->FocusedStateId = FGuid();
	}

	Data->LastOperationStatus = FString::Printf(TEXT("remove_state -> Removed '%s'"), *StateName);
	return BuildStateResponse(SessionId, Data);
}
