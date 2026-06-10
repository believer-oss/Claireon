// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_RenameState.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_RenameState::GetOperation() const { return TEXT("rename_state"); }

FString ClaireonStateTreeTool_RenameState::GetDescription() const
{
	return TEXT("Rename an existing state in the open State Tree editing session. Requires open session_id from statetree_open. Transactional. The new name must be unique among siblings on the same parent. The state's GUID is unchanged so transitions and bindings continue to resolve.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_RenameState::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state to rename."), true);
	Builder.AddString(TEXT("name"), TEXT("New name for the state."), true);
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_RenameState::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	{
		return MakeErrorResult(Error);
	}

	FString Name;
	if (!Arguments->TryGetStringField(TEXT("name"), Name) || Name.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: name"));
	}

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Rename State")));
	Data->StateTree->Modify();

	FString OldName = State->Name.ToString();
	State->Name = FName(*Name);
	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("rename_state -> '%s' -> '%s'"), *OldName, *Name);

	return BuildStateResponse(SessionId, Data);
}
