// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_RemoveGlobalTask.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_RemoveGlobalTask::GetName() const
{
	return TEXT("claireon.statetree_remove_global_task");
}

FString ClaireonStateTreeTool_RemoveGlobalTask::GetDescription() const
{
	return TEXT("Remove a global task from the State Tree in the open editing session. Requires open session_id from claireon.statetree_open. Transactional. Common pitfall: any bindings sourced from the task's outputs are dropped along with it; downstream consumers may revert to their authored defaults.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_RemoveGlobalTask::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the global task node to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_RemoveGlobalTask::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FGuid NodeId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("node_id"), NodeId, Error))
		return MakeErrorResult(Error);

	int32 Index = EditorData->GlobalTasks.IndexOfByPredicate([&NodeId](const FStateTreeEditorNode& N)
	{
		return N.ID == NodeId;
	});
	if (Index == INDEX_NONE)
		return MakeErrorResult(TEXT("Global task not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Global Task")));
	Data->StateTree->Modify();
	EditorData->GlobalTasks.RemoveAt(Index);

	Data->LastOperationStatus = TEXT("remove_global_task -> Removed global task");
	return BuildStateResponse(SessionId, Data);
}
