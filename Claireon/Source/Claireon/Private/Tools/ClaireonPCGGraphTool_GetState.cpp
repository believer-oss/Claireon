// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_GetState.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_GetState::GetName() const
{
	return TEXT("claireon.pcg_get_state");
}

FString ClaireonPCGGraphTool_GetState::GetDescription() const
{
	return TEXT("Get the current state of a PCG Graph editing session including the graph structure and focused node details.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_GetState::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_GetState::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	// get_state always returns full output
	Data->bSuppressOutput = false;
	return BuildStateResponse(SessionId, Data);
}
