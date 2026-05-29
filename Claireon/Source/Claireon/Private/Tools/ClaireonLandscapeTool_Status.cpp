// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeTool_Status.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeTool_Status::GetOperation() const { return TEXT("status"); }

FString ClaireonLandscapeTool_Status::GetDescription() const
{
    return TEXT("Get the current state of a landscape editing session, including layers, materials, and bounds.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeTool_Status::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonLandscapeTool_Status::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	return BuildStateResponse(SessionId, Data);
}
