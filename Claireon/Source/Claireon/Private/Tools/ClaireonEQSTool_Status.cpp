// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonEQSTool_Status.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonEQSTool_Status::GetName() const
{
	return TEXT("claireon.eqs_status");
}

FString ClaireonEQSTool_Status::GetDescription() const
{
	return TEXT("Get the current state of an EQS editing session including all options, generators, and tests.");
}

TSharedPtr<FJsonObject> ClaireonEQSTool_Status::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonEQSTool_Status::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FEQSEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	return BuildStateResponse(SessionId, Data);
}
