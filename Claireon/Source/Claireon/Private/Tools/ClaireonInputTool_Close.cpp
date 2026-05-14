// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_Close.h"
#include "Tools/ClaireonInputTool_Save.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSessionManager.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_Close::GetOperation() const { return TEXT("close"); }

FString ClaireonInputTool_Close::GetDescription() const
{
	return TEXT("Close the Input edit session, optionally saving first.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddBoolean(TEXT("save_first"), TEXT("Save the asset before closing the session."));
	return Builder.Build();
}

FToolResult ClaireonInputTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FInputEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	bool bSaveFirst = false;
	Arguments->TryGetBoolField(TEXT("save_first"), bSaveFirst);

	if (bSaveFirst)
	{
		ClaireonInputTool_Save SaveTool;
		TSharedPtr<FJsonObject> SaveArgs = MakeShared<FJsonObject>();
		SaveArgs->SetStringField(TEXT("session_id"), SessionId);
		SaveTool.Execute(SaveArgs);
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> RespData = MakeShared<FJsonObject>();
	RespData->SetStringField(TEXT("session_id"), SessionId);
	RespData->SetBoolField(TEXT("closed"), true);
	return MakeSuccessResult(RespData, FString::Printf(TEXT("Session closed: %s"), *SessionId));
}
