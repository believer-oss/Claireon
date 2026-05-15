// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_AddEvaluator.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_AddEvaluator::GetName() const
{
	return TEXT("claireon.statetree_add_evaluator");
}

FString ClaireonStateTreeTool_AddEvaluator::GetDescription() const
{
	return TEXT("Add a global evaluator node to the State Tree.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_AddEvaluator::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node_type"), TEXT("Name of the evaluator struct."), true);
	Builder.AddObject(TEXT("properties"), TEXT("Optional map of property_name -> string value."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_AddEvaluator::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Evaluator")));
	Data->StateTree->Modify();
	EditorData->Evaluators.Add(MoveTemp(NewNode));

	Data->LastOperationStatus = FString::Printf(TEXT("add_evaluator -> Added global evaluator %s"), *NodeType);
	return BuildStateResponse(SessionId, Data);
}
