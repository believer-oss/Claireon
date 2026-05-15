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
	return TEXT("Add a child state to an existing parent state.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_AddState::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parent_state_id"), TEXT("GUID of the parent state."), true);
	Builder.AddString(TEXT("name"), TEXT("Name for the new state."), true);
	Builder.AddString(TEXT("state_type"), TEXT("State type: State, Group, Linked, LinkedAsset, Subtree. Defaults to State."));
	Builder.AddString(TEXT("insert_after"), TEXT("Optional GUID of a sibling state. The new state is inserted after this sibling."));
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

	// insert_after: optionally reorder the newly-appended child
	FGuid InsertAfterId;
	{
		FString InsertAfterStr;
		if (Arguments->TryGetStringField(TEXT("insert_after"), InsertAfterStr) && !InsertAfterStr.IsEmpty())
		{
			FGuid::Parse(InsertAfterStr, InsertAfterId);
		}
	}
	if (InsertAfterId.IsValid())
	{
		int32 AfterIndex = ParentState->Children.IndexOfByPredicate(
			[&InsertAfterId](const UStateTreeState* S)
		{
			return S && S->ID == InsertAfterId;
		});
		if (AfterIndex == INDEX_NONE)
		{
			return MakeErrorResult(FString::Printf(TEXT("insert_after state '%s' not found among children of '%s'"),
				*InsertAfterId.ToString(), *ParentState->Name.ToString()));
		}
		int32 LastIndex = ParentState->Children.Num() - 1;
		if (LastIndex != AfterIndex + 1)
		{
			TObjectPtr<UStateTreeState> NewChild = ParentState->Children[LastIndex];
			ParentState->Children.RemoveAt(LastIndex, EAllowShrinking::No);
			ParentState->Children.Insert(NewChild, AfterIndex + 1);
		}
	}

	Data->PushHistory();
	Data->FocusedStateId = NewState.ID;
	Data->LastOperationStatus = FString::Printf(TEXT("add_state -> Added '%s' to '%s'"), *Name, *ParentState->Name.ToString());

	return BuildStateResponse(SessionId, Data);
}
