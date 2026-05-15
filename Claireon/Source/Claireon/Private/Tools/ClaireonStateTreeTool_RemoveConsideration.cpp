// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_RemoveConsideration.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_RemoveConsideration::GetName() const
{
	return TEXT("claireon.statetree_remove_consideration");
}

FString ClaireonStateTreeTool_RemoveConsideration::GetDescription() const
{
	return TEXT("Remove a consideration node from a state.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_RemoveConsideration::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state."), true);
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the consideration node to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_RemoveConsideration::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FGuid StateId, NodeId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("node_id"), NodeId, Error))
		return MakeErrorResult(Error);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	int32 Index = State->Considerations.IndexOfByPredicate([&NodeId](const FStateTreeEditorNode& N)
	{
		return N.ID == NodeId;
	});
	if (Index == INDEX_NONE)
		return MakeErrorResult(TEXT("Consideration not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Consideration")));
	Data->StateTree->Modify();
	State->Considerations.RemoveAt(Index);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = TEXT("remove_consideration -> Removed consideration");
	return BuildStateResponse(SessionId, Data);
}
