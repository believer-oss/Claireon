// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonFoliageTool_Close.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSessionManager.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonFoliageTool_Close::GetName() const
{
	return TEXT("claireon.foliage_close");
}

FString ClaireonFoliageTool_Close::GetDescription() const
{
	return TEXT("Close a foliage editing session.");
}

TSharedPtr<FJsonObject> ClaireonFoliageTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonFoliageTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FFoliageEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(ResultData, TEXT("Session closed"));
}
