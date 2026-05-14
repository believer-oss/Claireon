// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_SetStateEnabled.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_SetStateEnabled::GetOperation() const { return TEXT("set_state_enabled"); }

FString ClaireonStateTreeTool_SetStateEnabled::GetDescription() const
{
	return TEXT("Enable or disable a state in the open State Tree editing session. Requires open session_id from statetree_open. Transactional. Disabled states are skipped during selection without affecting the rest of the tree's structure. Use to temporarily quarantine a state without deleting its definition.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_SetStateEnabled::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("state_id"), TEXT("GUID of the state."), true);
	Builder.AddBoolean(TEXT("enabled"), TEXT("True to enable, false to disable. Defaults to true."));
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_SetStateEnabled::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	bool bEnabled = true;
	Arguments->TryGetBoolField(TEXT("enabled"), bEnabled);

	UStateTreeState* State = ClaireonStateTreeHelpers::FindStateById(EditorData, StateId);
	if (!State)
		return MakeErrorResult(TEXT("State not found"));

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set State Enabled")));
	Data->StateTree->Modify();
	State->bEnabled = bEnabled;

	Data->FocusedStateId = StateId;
	Data->LastOperationStatus = FString::Printf(TEXT("set_state_enabled -> '%s' %s"), *State->Name.ToString(), bEnabled ? TEXT("enabled") : TEXT("disabled"));

	return BuildStateResponse(SessionId, Data);
}
