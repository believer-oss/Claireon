// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_Close.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "ClaireonSessionManager.h"

#include "Dom/JsonObject.h"

FString FClaireonMetaSoundTool_Close::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_Close::GetOperation() const { return TEXT("close"); }

FString FClaireonMetaSoundTool_Close::GetDescription() const
{
	return TEXT("Close a UMetaSoundSource editing session and release the lock and builder handle.");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by metasound_open"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: session_id"));
	}

	FClaireonSessionManager::Get().CloseSession(SessionId);
	ClaireonAudioSessionRegistry::ReleaseSession(SessionId, ESoundCohort::MetaSound);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), SessionId);
	return MakeSuccessResult(Data, FString::Printf(TEXT("MetaSound session closed: %s"), *SessionId));
}
