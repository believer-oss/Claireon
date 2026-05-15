// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_AddConsideration.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeEditorNode.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_AddConsideration::GetName() const
{
	return TEXT("claireon.statetree_add_consideration");
}

FString ClaireonStateTreeTool_AddConsideration::GetDescription() const
{
	return TEXT("Add a utility consideration to a state.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_AddConsideration::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state to add the consideration to."), true);
	Builder.AddString(TEXT("node_type"), TEXT("Name of the consideration struct."), true);
	Builder.AddObject(TEXT("properties"), TEXT("Optional map of property_name -> string value to set on the new consideration."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_AddConsideration::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
	{
		ClaireonStateTreeEditInternal::SetInitialProperties(NewNode, *PropsObj, State);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Consideration")));
	Data->StateTree->Modify();
	State->Considerations.Add(MoveTemp(NewNode));

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("add_consideration -> Added %s to '%s'"), *NodeType, *State->Name.ToString());
	return BuildStateResponse(SessionId, Data);
}
