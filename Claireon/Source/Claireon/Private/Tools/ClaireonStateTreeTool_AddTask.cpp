// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_AddTask.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeSchema.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_AddTask::GetName() const
{
	return TEXT("claireon.statetree_add_task");
}

FString ClaireonStateTreeTool_AddTask::GetDescription() const
{
	return TEXT("Add a task node to a state.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_AddTask::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state to add the task to."), true);
	Builder.AddString(TEXT("node_type"), TEXT("Name of the task struct (e.g. 'FStateTreeMoveToTask')."), true);
	Builder.AddObject(TEXT("properties"), TEXT("Optional map of property_name -> string value to set on the new task."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_AddTask::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString NodeType;
	if (!Arguments->TryGetStringField(TEXT("node_type"), NodeType))
		return MakeErrorResult(TEXT("Missing parameter: node_type"));

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(NodeType, Error);
	if (!NodeStruct)
		return MakeErrorResult(Error);

	FStateTreeEditorNode NewNode;
	if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, State, Error))
	{
		return MakeErrorResult(Error);
	}

	// Set initial properties
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
	{
		ClaireonStateTreeEditInternal::SetInitialProperties(NewNode, *PropsObj, State);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Task")));
	Data->StateTree->Modify();

	// Check schema for single vs multiple tasks
	const UStateTreeSchema* Schema = Data->StateTree->GetSchema();
	if (Schema && !Schema->AllowMultipleTasks())
	{
		State->SingleTask = MoveTemp(NewNode);
	}
	else
	{
		State->Tasks.Add(MoveTemp(NewNode));
	}

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("add_task -> Added %s to '%s'"), *NodeType, *State->Name.ToString());

	return BuildStateResponse(SessionId, Data);
}
