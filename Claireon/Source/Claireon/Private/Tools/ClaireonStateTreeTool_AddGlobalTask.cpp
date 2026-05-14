// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_AddGlobalTask.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_AddGlobalTask::GetOperation() const { return TEXT("add_global_task"); }

FString ClaireonStateTreeTool_AddGlobalTask::GetDescription() const
{
	return TEXT("Add a global task to the State Tree in the open editing session. Requires open session_id from statetree_open. Transactional. Global tasks run for the lifetime of the State Tree instance regardless of selected state. The node_type must be a registered FStateTreeTaskBase subclass.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_AddGlobalTask::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node_type"), TEXT("Name of the task struct."), true);
	Builder.AddObject(TEXT("properties"), TEXT("Optional map of property_name -> string value."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_AddGlobalTask::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString NodeType;
	if (!Arguments->TryGetStringField(TEXT("node_type"), NodeType))
		return MakeErrorResult(TEXT("Missing parameter: node_type"));

	UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(NodeType, Error);
	if (!NodeStruct)
		return MakeErrorResult(Error);

	FStateTreeEditorNode NewNode;
	if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, EditorData, Error))
	{
		return MakeErrorResult(Error);
	}

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
	{
		ClaireonStateTreeEditInternal::SetInitialProperties(NewNode, *PropsObj, EditorData);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Global Task")));
	Data->StateTree->Modify();
	const FString NewIdStr = NewNode.ID.ToString(EGuidFormats::DigitsWithHyphens);
	EditorData->GlobalTasks.Add(MoveTemp(NewNode));

	Data->LastOperationStatus = FString::Printf(TEXT("add_global_task -> Added global task %s"), *NodeType);
	return BuildStateResponse(SessionId, Data, FStringView(TEXT("global_task_id")), FStringView(NewIdStr));
}
