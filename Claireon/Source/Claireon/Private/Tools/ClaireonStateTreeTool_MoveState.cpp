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
	return TEXT("Move an existing state under a different parent in the open State Tree editing session, optionally specifying placement among the new parent's children. Requires open session_id from claireon.statetree_open. Transactional. Common pitfall: cycles are rejected; the new parent must not be the state itself or any of its descendants.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_MoveState::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state to move."), true);
	Builder.AddString(TEXT("new_parent_id"), TEXT("GUID of the new parent state."), true);
	Builder.AddInteger(TEXT("index"), TEXT("Optional explicit insertion index among the new parent's children. Out-of-range values clamp to [0, num_children]. Highest precedence: wins over insert_before and insert_after."));
	Builder.AddString(TEXT("insert_before"), TEXT("Optional GUID of a sibling under the new parent; insert the moved state before it. Loses to index, wins over insert_after."));
	Builder.AddString(TEXT("insert_after"), TEXT("Optional GUID of a sibling under the new parent; insert the moved state after it. Lowest precedence (loses to index and insert_before)."));
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

	// Reject self-referential placement before mutating any container.
	{
		FString InsertBeforeStr;
		if (Arguments->TryGetStringField(TEXT("insert_before"), InsertBeforeStr) && !InsertBeforeStr.IsEmpty())
		{
			FGuid PeekGuid;
			if (FGuid::Parse(InsertBeforeStr, PeekGuid) && PeekGuid == State->ID)
			{
				return MakeErrorResult(TEXT("insert_before cannot reference the state being moved"));
			}
		}
		FString InsertAfterStr;
		if (Arguments->TryGetStringField(TEXT("insert_after"), InsertAfterStr) && !InsertAfterStr.IsEmpty())
		{
			FGuid PeekGuid;
			if (FGuid::Parse(InsertAfterStr, PeekGuid) && PeekGuid == State->ID)
			{
				return MakeErrorResult(TEXT("insert_after cannot reference the state being moved"));
			}
		}
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

	// Resolve placement under the new parent (after removal, so 'index' clamps to the
	// post-removal child count and sibling-GUID lookups can't accidentally match the
	// state being moved -- matters for same-parent reorders).
	int32 TargetIndex = NewParent->Children.Num();
	FString ResolutionNote;
	FString ResolveError;
	if (!ClaireonStateTreeEditInternal::ResolveInsertionIndex(
			NewParent->Children, Arguments, NewParent->Children.Num(),
			TargetIndex, ResolutionNote, ResolveError))
	{
		// Restore: put state back in old parent to avoid orphaning.
		if (OldParent)
		{
			OldParent->Children.Add(State);
		}
		else
		{
			EditorData->SubTrees.Add(State);
		}
		return MakeErrorResult(ResolveError);
	}

	NewParent->Children.Insert(State, TargetIndex);
	State->Rename(nullptr, NewParent);

	Data->FocusedStateId = StateId;
	if (!ResolutionNote.IsEmpty())
	{
		Data->LastOperationStatus = FString::Printf(
			TEXT("move_state -> Moved '%s' to '%s' at index %d (%s)"),
			*State->Name.ToString(), *NewParent->Name.ToString(), TargetIndex, *ResolutionNote);
	}
	else
	{
		Data->LastOperationStatus = FString::Printf(
			TEXT("move_state -> Moved '%s' to '%s' at index %d"),
			*State->Name.ToString(), *NewParent->Name.ToString(), TargetIndex);
	}

	return BuildStateResponse(SessionId, Data);
}
