// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundCueTool_Close.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "ClaireonSessionManager.h"

#include "Dom/JsonObject.h"

FString FClaireonSoundCueTool_Close::GetCategory() const { return TEXT("soundcue"); }
FString FClaireonSoundCueTool_Close::GetOperation() const { return TEXT("close"); }

FString FClaireonSoundCueTool_Close::GetDescription() const
{
	return TEXT("Close a USoundCue editing session, releasing the asset lock and cleaning up tool data. "
				"Call after all edits are complete to unblock other callers. "
				"The session_id is invalidated after this call; subsequent ops must open a new session.");
}

TSharedPtr<FJsonObject> FClaireonSoundCueTool_Close::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by soundcue_open"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundCueTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	ClaireonAudioSessionRegistry::ReleaseSession(SessionId, ESoundCohort::SoundCue);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), SessionId);
	return MakeSuccessResult(Data, FString::Printf(TEXT("SoundCue session closed: %s"), *SessionId));
}
