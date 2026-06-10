// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_Close.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSessionManager.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_Close::GetOperation() const { return TEXT("close"); }

FString ClaireonMaterialTool_Close::GetDescription() const
{
    return TEXT("Close an open material editing session and release the asset lock, clearing in-session graph state.");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: session_id"));
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);

	TSharedPtr<FJsonObject> Resp = MakeShared<FJsonObject>();
	Resp->SetStringField(TEXT("session_id"), SessionId);
	Resp->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(Resp, FString::Printf(TEXT("Session closed: %s"), *SessionId));
}
