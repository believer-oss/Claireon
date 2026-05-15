// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_RemoveEnterCondition.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_RemoveEnterCondition::GetName() const
{
	return TEXT("claireon.statetree_remove_enter_condition");
}

FString ClaireonStateTreeTool_RemoveEnterCondition::GetDescription() const
{
	return TEXT("Remove an enter-condition node from a state.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_RemoveEnterCondition::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state."), true);
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the condition node to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_RemoveEnterCondition::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	int32 Index = State->EnterConditions.IndexOfByPredicate([&NodeId](const FStateTreeEditorNode& N)
	{
		return N.ID == NodeId;
	});
	if (Index == INDEX_NONE)
		return MakeErrorResult(TEXT("Condition not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Enter Condition")));
	Data->StateTree->Modify();
	State->EnterConditions.RemoveAt(Index);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = TEXT("remove_enter_condition -> Removed condition");
	return BuildStateResponse(SessionId, Data);
}
