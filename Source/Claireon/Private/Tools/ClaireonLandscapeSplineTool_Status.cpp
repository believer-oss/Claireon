// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeSplineTool_Status.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeSplineTool_Status::GetOperation() const { return TEXT("spline_status"); }

FString ClaireonLandscapeSplineTool_Status::GetDescription() const
{
    return TEXT("Get the current spline state (control points, segments) for the open landscape spline editing session.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeSplineTool_Status::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonLandscapeSplineTool_Status::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeSplineEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	return BuildStateResponse(SessionId, Data);
}
