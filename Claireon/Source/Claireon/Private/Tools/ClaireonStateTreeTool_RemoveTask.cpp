// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_RemoveTask.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_RemoveTask::GetName() const
{
	return TEXT("claireon.statetree_remove_task");
}

FString ClaireonStateTreeTool_RemoveTask::GetDescription() const
{
	return TEXT("Remove a task node from a state in the open State Tree editing session. Requires open session_id from claireon.statetree_open. Transactional. Bindings targeting the removed task's properties are dropped. Transitions with trigger=OnTaskCompleted on this task become unreachable; remove or retarget them as needed.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_RemoveTask::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state."), true);
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the task node to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_RemoveTask::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Task")));
	Data->StateTree->Modify();

	// Check SingleTask
	if (State->SingleTask.ID == NodeId)
	{
		State->SingleTask = FStateTreeEditorNode();
		Data->LastOperationStatus = TEXT("remove_task -> Removed single task");
	}
	else
	{
		int32 Index = State->Tasks.IndexOfByPredicate([&NodeId](const FStateTreeEditorNode& N)
		{
			return N.ID == NodeId;
		});
		if (Index == INDEX_NONE)
			return MakeErrorResult(TEXT("Task node not found in state"));
		State->Tasks.RemoveAt(Index);
		Data->LastOperationStatus = TEXT("remove_task -> Removed task");
	}

	Data->FocusedStateId = StateId;
	return BuildStateResponse(SessionId, Data);
}
