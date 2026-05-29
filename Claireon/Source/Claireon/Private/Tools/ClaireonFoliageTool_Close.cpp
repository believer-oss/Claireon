// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonFoliageTool_Close.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSessionManager.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonFoliageTool_Close::GetOperation() const { return TEXT("close"); }

FString ClaireonFoliageTool_Close::GetDescription() const
{
    return TEXT("Close a foliage editing session, releasing the foliage actor lock and clearing in-session state. Session-mode tool: open via foliage_open first.");
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
