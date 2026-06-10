// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_RemoveEvaluator.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_RemoveEvaluator::GetOperation() const { return TEXT("remove_evaluator"); }

FString ClaireonStateTreeTool_RemoveEvaluator::GetDescription() const
{
	return TEXT("Remove a global evaluator from the State Tree in the open editing session. Requires open session_id from statetree_open. Transactional. Common pitfall: any bindings sourced from the evaluator's outputs are dropped; downstream consumers may revert to their authored defaults.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_RemoveEvaluator::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the evaluator node to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_RemoveEvaluator::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	int32 Index = EditorData->Evaluators.IndexOfByPredicate([&NodeId](const FStateTreeEditorNode& N)
	{
		return N.ID == NodeId;
	});
	if (Index == INDEX_NONE)
		return MakeErrorResult(TEXT("Evaluator not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Evaluator")));
	Data->StateTree->Modify();
	EditorData->Evaluators.RemoveAt(Index);

	Data->LastOperationStatus = TEXT("remove_evaluator -> Removed global evaluator");
	return BuildStateResponse(SessionId, Data);
}
