// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_MoveState.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_MoveState::GetName() const
{
	return TEXT("claireon.statetree_move_state");
}

FString ClaireonStateTreeTool_MoveState::GetDescription() const
{
	return TEXT("Move an existing state under a different parent, optionally inserting after a sibling.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_MoveState::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state to move."), true);
	Builder.AddString(TEXT("new_parent_id"), TEXT("GUID of the new parent state."), true);
	Builder.AddString(TEXT("insert_after"), TEXT("Optional GUID of a sibling; insert the moved state after it."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_MoveState::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FGuid StateId, NewParentId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("new_parent_id"), NewParentId, Error))
		return MakeErrorResult(Error);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	UStateTreeState* NewParent = ClaireonStateTreeHelpers::FindStateById(EditorData, NewParentId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));
	if (!NewParent)
		return MakeErrorResult(TEXT("New parent state not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Move State")));
	Data->StateTree->Modify();

	// Parse optional insert_after
	FGuid InsertAfterId;
	{
		FString InsertAfterStr;
		if (Arguments->TryGetStringField(TEXT("insert_after"), InsertAfterStr) && !InsertAfterStr.IsEmpty())
		{
			FGuid::Parse(InsertAfterStr, InsertAfterId);
		}
	}
	if (InsertAfterId.IsValid() && InsertAfterId == State->ID)
	{
		return MakeErrorResult(TEXT("insert_after cannot reference the state being moved"));
	}

	// Remove from old parent
	UStateTreeState* OldParent = Cast<UStateTreeState>(State->GetOuter());
	if (OldParent)
	{
		OldParent->Children.Remove(State);
	}
	else
	{
		EditorData->SubTrees.Remove(State);
	}

	// Add to new parent
	if (InsertAfterId.IsValid())
	{
		int32 AfterIndex = NewParent->Children.IndexOfByPredicate(
			[&InsertAfterId](const UStateTreeState* S)
		{
			return S && S->ID == InsertAfterId;
		});
		if (AfterIndex == INDEX_NONE)
		{
			// Restore: put state back in old parent to avoid orphaning
			if (OldParent)
			{
				OldParent->Children.Add(State);
			}
			else
			{
				EditorData->SubTrees.Add(State);
			}
			return MakeErrorResult(FString::Printf(TEXT("insert_after state '%s' not found among children of '%s'"),
				*InsertAfterId.ToString(), *NewParent->Name.ToString()));
		}
		NewParent->Children.Insert(State, AfterIndex + 1);
	}
	else
	{
		NewParent->Children.Add(State);
	}
	State->Rename(nullptr, NewParent);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("move_state -> Moved '%s' to '%s'"), *State->Name.ToString(), *NewParent->Name.ToString());

	return BuildStateResponse(SessionId, Data);
}
