// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_Close.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSessionManager.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_Close::GetOperation() const { return TEXT("instance_close"); }

FString ClaireonMaterialInstanceTool_Close::GetDescription() const
{
	return TEXT("Close a UMaterialInstanceConstant editing session.");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FMaterialInstanceEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	// HandleSessionClosed will remove the ToolData entry.

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("session_id"), SessionId);
	Resp->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(Resp, FString::Printf(TEXT("Session closed: %s"), *SessionId));
}
