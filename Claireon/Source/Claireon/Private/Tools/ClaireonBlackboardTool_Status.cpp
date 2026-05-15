// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlackboardTool_Status.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlackboardTool_Status::GetName() const
{
	return TEXT("claireon.blackboard_status");
}

FString ClaireonBlackboardTool_Status::GetDescription() const
{
	return TEXT("Get the current state of a Blackboard editing session including all keys and their types.");
}

TSharedPtr<FJsonObject> ClaireonBlackboardTool_Status::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonBlackboardTool_Status::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FBlackboardEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	return BuildStateResponse(SessionId, Data);
}
