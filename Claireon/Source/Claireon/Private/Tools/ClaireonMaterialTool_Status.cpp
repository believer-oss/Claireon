// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_Status.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_Status::GetName() const
{
	return TEXT("claireon.material_status");
}

FString ClaireonMaterialTool_Status::GetDescription() const
{
	return TEXT("Get current state snapshot of an open material editing session.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_Status::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_Status::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}
	return BuildStateResponse(SessionId, Data);
}
