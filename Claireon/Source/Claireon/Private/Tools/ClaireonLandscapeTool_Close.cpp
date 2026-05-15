// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeTool_Close.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSessionManager.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeTool_Close::GetName() const
{
	return TEXT("claireon.landscape_close");
}

FString ClaireonLandscapeTool_Close::GetDescription() const
{
	return TEXT("Close a landscape editing session.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonLandscapeTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeEditToolData* Data = nullptr;
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
