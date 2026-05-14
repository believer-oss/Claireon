// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_SetStateType.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_SetStateType::GetName() const
{
	return TEXT("claireon.statetree_set_state_type");
}

FString ClaireonStateTreeTool_SetStateType::GetDescription() const
{
	return TEXT("Set the type of a state (State, Group, Linked, LinkedAsset, Subtree) in the open State Tree editing session. Requires open session_id from claireon.statetree_open. Transactional. Common pitfall: switching from a leaf type to Group type may invalidate task/condition placements that don't apply to the new type.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_SetStateType::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state."), true);
	Builder.AddString(TEXT("type"), TEXT("State type: State, Group, Linked, LinkedAsset, Subtree."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_SetStateType::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString TypeStr;
	if (!Arguments->TryGetStringField(TEXT("type"), TypeStr))
		return MakeErrorResult(TEXT("Missing parameter: type"));

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set State Type")));
	Data->StateTree->Modify();
	State->Type = ClaireonStateTreeEditInternal::ParseStateType(TypeStr);

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("set_state_type -> '%s' type set to %s"), *State->Name.ToString(), *TypeStr);

	return BuildStateResponse(SessionId, Data);
}
