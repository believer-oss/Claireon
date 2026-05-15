// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_Status.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_Status::GetName() const
{
	return TEXT("claireon.niagara_status");
}

FString ClaireonNiagaraTool_Status::GetDescription() const
{
	return TEXT("Return the current state of the Niagara edit session.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_Status::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_Status::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FNiagaraEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}
	return BuildStateResponse(SessionId, Data);
}
