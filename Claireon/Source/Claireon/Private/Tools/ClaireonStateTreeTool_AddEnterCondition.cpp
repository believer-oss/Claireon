// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_AddEnterCondition.h"
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

FString ClaireonStateTreeTool_AddEnterCondition::GetName() const
{
	return TEXT("claireon.statetree_add_enter_condition");
}

FString ClaireonStateTreeTool_AddEnterCondition::GetDescription() const
{
	return TEXT("Add an enter-condition node to a state in the open State Tree editing session. Requires open session_id from claireon.statetree_open. Transactional. Enter conditions gate state entry; the state is skipped at selection time when any enter condition fails. Configure properties via claireon.statetree_set_node_property.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_AddEnterCondition::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state to add the condition to."), true);
	Builder.AddString(TEXT("node_type"), TEXT("Name of the condition struct."), true);
	Builder.AddString(TEXT("expression_operand"), TEXT("Operand: And (default), Or, Copy."));
	Builder.AddInteger(TEXT("expression_indent"), TEXT("Expression indent level (0-4)."));
	Builder.AddObject(TEXT("properties"), TEXT("Optional map of property_name -> string value to set on the new condition."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_AddEnterCondition::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	// Set expression operand and indent
	FString OperandStr = TEXT("And");
	Arguments->TryGetStringField(TEXT("expression_operand"), OperandStr);
	if (OperandStr == TEXT("Or"))
		NewNode.ExpressionOperand = EStateTreeExpressionOperand::Or;
	else if (OperandStr == TEXT("Copy"))
		NewNode.ExpressionOperand = EStateTreeExpressionOperand::Copy;
	else
		NewNode.ExpressionOperand = EStateTreeExpressionOperand::And;

	int32 Indent = 0;
	if (Arguments->HasField(TEXT("expression_indent")))
	{
		Indent = FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("expression_indent"))), 0, 4);
	}
	NewNode.ExpressionIndent = static_cast<uint8>(Indent);

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
	{
		ClaireonStateTreeEditInternal::SetInitialProperties(NewNode, *PropsObj, State);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Enter Condition")));
	Data->StateTree->Modify();
	const FString NewIdStr = NewNode.ID.ToString(EGuidFormats::DigitsWithHyphens);
	State->EnterConditions.Add(MoveTemp(NewNode));

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("add_enter_condition -> Added %s to '%s'"), *NodeType, *State->Name.ToString());

	return BuildStateResponse(SessionId, Data, FStringView(TEXT("enter_condition_id")), FStringView(NewIdStr));
}
