// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_AddState.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_AddState::GetName() const
{
	return TEXT("claireon.statetree_add_state");
}

FString ClaireonStateTreeTool_AddState::GetDescription() const
{
	return TEXT("Add a child state to an existing parent state in the open State Tree editing session. Requires open session_id from claireon.statetree_open. Transactional. By default the new state is appended as the last child; use index / insert_before / insert_after to control placement. Returns the new state's GUID for downstream operations.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_AddState::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parent_state_id"), TEXT("GUID of the parent state."), true);
	Builder.AddString(TEXT("name"), TEXT("Name for the new state."), true);
	Builder.AddString(TEXT("state_type"), TEXT("State type: State, Group, Linked, LinkedAsset, Subtree. Defaults to State."));
	Builder.AddInteger(TEXT("index"), TEXT("Optional explicit insertion index among siblings. Out-of-range values clamp to [0, num_children]. Highest precedence: wins over insert_before and insert_after."));
	Builder.AddString(TEXT("insert_before"), TEXT("Optional GUID of a sibling state. The new state is inserted before this sibling. Loses to index, wins over insert_after."));
	Builder.AddString(TEXT("insert_after"), TEXT("Optional GUID of a sibling state. The new state is inserted after this sibling. Lowest precedence (loses to index and insert_before)."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_AddState::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FGuid ParentStateId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("parent_state_id"), ParentStateId, Error))
	{
		return MakeErrorResult(Error);
	}

	FString Name;
	if (!Arguments->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: name"));
	}

	FString StateTypeStr = TEXT("State");
	Arguments->TryGetStringField(TEXT("state_type"), StateTypeStr);

	UStateTreeState* ParentState = ClaireonStateTreeHelpers::FindStateById(EditorData, ParentStateId);
	if (!ParentState)
	{
		return MakeErrorResult(FString::Printf(TEXT("Parent state not found: %s"), *ParentStateId.ToString()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add State")));
	Data->StateTree->Modify();

	UStateTreeState& NewState = ParentState->AddChildState(FName(*Name), ClaireonStateTreeEditInternal::ParseStateType(StateTypeStr));

	// Resolve placement using D4 precedence (index > insert_before > insert_after).
	// The new state was just appended at Children.Num()-1; resolve target against the
	// array of pre-existing siblings (size = Children.Num()-1) and re-insert if needed.
	const int32 AppendedIndex = ParentState->Children.Num() - 1;
	int32 TargetIndex = AppendedIndex;
	FString ResolutionNote;
	FString ResolveError;
	{
		// Build a temporary view of the pre-existing siblings so the helper's GUID
		// lookup cannot accidentally match the just-appended state, and so 'index'
		// clamps to the pre-add range [0, AppendedIndex].
		TArray<TObjectPtr<UStateTreeState>> ExistingSiblings(ParentState->Children.GetData(), AppendedIndex);
		if (!ClaireonStateTreeEditInternal::ResolveInsertionIndex(
				ExistingSiblings, Arguments, AppendedIndex,
				TargetIndex, ResolutionNote, ResolveError))
		{
			// Roll back the append so the asset is left untouched on a bad sibling GUID.
			ParentState->Children.RemoveAt(AppendedIndex, EAllowShrinking::No);
			return MakeErrorResult(ResolveError);
		}
	}
	if (TargetIndex != AppendedIndex)
	{
		TObjectPtr<UStateTreeState> NewChild = ParentState->Children[AppendedIndex];
		ParentState->Children.RemoveAt(AppendedIndex, EAllowShrinking::No);
		ParentState->Children.Insert(NewChild, TargetIndex);
	}

	Data->PushHistory();
	Data->FocusedStateId = NewState.ID;
	if (!ResolutionNote.IsEmpty())
	{
		Data->LastOperationStatus = FString::Printf(
			TEXT("add_state -> Added '%s' to '%s' at index %d (%s)"),
			*Name, *ParentState->Name.ToString(), TargetIndex, *ResolutionNote);
	}
	else
	{
		Data->LastOperationStatus = FString::Printf(
			TEXT("add_state -> Added '%s' to '%s' at index %d"),
			*Name, *ParentState->Name.ToString(), TargetIndex);
	}

	const FString NewIdStr = NewState.ID.ToString(EGuidFormats::DigitsWithHyphens);
	return BuildStateResponse(SessionId, Data, FStringView(TEXT("state_id")), FStringView(NewIdStr));
}
