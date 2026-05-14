// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeTool_Status.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonStateTreeTool_Status::GetOperation() const { return TEXT("status"); }

FString ClaireonStateTreeTool_Status::GetDescription() const
{
	return TEXT("Return the current authoring snapshot of the open State Tree editing session: states, transitions, evaluators, global tasks, parameters, and any unsaved-change indicator. Requires open session_id from statetree_open. Read-only. Use to verify state before save/close.");
}

TSharedPtr<FJsonObject> ClaireonStateTreeTool_Status::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonStateTreeTool_Status::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FStateTreeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}
	return BuildStateResponse(SessionId, Data);
}
