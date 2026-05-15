// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_AddTransitionCondition.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "StateTreeTypes.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_AddTransitionCondition::GetName() const
{
	return TEXT("claireon.statetree_add_transition_condition");
}

FString ClaireonStateTreeTool_AddTransitionCondition::GetDescription() const
{
	return TEXT("Add a condition node to a transition.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_AddTransitionCondition::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state."), true);
	Builder.AddString(TEXT("transition_id"), TEXT("GUID of the transition."), true);
	Builder.AddString(TEXT("node_type"), TEXT("Name of the condition struct."), true);
	Builder.AddString(TEXT("expression_operand"), TEXT("Operand: And (default), Or, Copy."));
	Builder.AddObject(TEXT("properties"), TEXT("Optional map of property_name -> string value."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_AddTransitionCondition::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FGuid StateId, TransitionId;
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("state_id"), StateId, Error))
		return MakeErrorResult(Error);
	if (!ClaireonStateTreeEditInternal::ParseGuidParam(Arguments, TEXT("transition_id"), TransitionId, Error))
		return MakeErrorResult(Error);

	FString NodeType;
	if (!Arguments->TryGetStringField(TEXT("node_type"), NodeType))
		return MakeErrorResult(TEXT("Missing parameter: node_type"));

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FStateTreeTransition* Trans = ClaireonStateTreeHelpers::FindTransitionById(State, TransitionId);
	if (!Trans)
		return MakeErrorResult(TEXT("Transition not found"));

	UScriptStruct* NodeStruct = ClaireonStateTreeHelpers::ResolveNodeStruct(NodeType, Error);
	if (!NodeStruct)
		return MakeErrorResult(Error);

	FStateTreeEditorNode NewNode;
	if (!ClaireonStateTreeHelpers::CreateEditorNode(NewNode, NodeStruct, State, Error))
	{
		return MakeErrorResult(Error);
	}

	FString OperandStr = TEXT("And");
	Arguments->TryGetStringField(TEXT("expression_operand"), OperandStr);
	if (OperandStr == TEXT("Or"))
		NewNode.ExpressionOperand = EStateTreeExpressionOperand::Or;
	else if (OperandStr == TEXT("Copy"))
		NewNode.ExpressionOperand = EStateTreeExpressionOperand::Copy;

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
	{
		ClaireonStateTreeEditInternal::SetInitialProperties(NewNode, *PropsObj, State);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Transition Condition")));
	Data->StateTree->Modify();
	Trans->Conditions.Add(MoveTemp(NewNode));

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("add_transition_condition -> Added %s"), *NodeType);
	return BuildStateResponse(SessionId, Data);
}
