// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_Status.h"
#include "Tools/FToolSchemaBuilder.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_Status::GetOperation() const { return TEXT("instance_status"); }

FString ClaireonMaterialInstanceTool_Status::GetDescription() const
{
	return TEXT("Return the current session state of a UMaterialInstanceConstant edit session.");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_Status::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_Status::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialInstanceEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	return BuildStateResponse(SessionId, Data);
}
