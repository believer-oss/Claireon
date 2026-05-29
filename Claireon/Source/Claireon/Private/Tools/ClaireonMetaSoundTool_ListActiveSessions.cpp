// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_ListActiveSessions.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "ClaireonSessionManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString FClaireonMetaSoundTool_ListActiveSessions::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_ListActiveSessions::GetOperation() const { return TEXT("list_active_sessions"); }

FString FClaireonMetaSoundTool_ListActiveSessions::GetDescription() const
{
	return TEXT("Return all currently-active MetaSound editing sessions held under the 'audio_edit' "
				"lock. Use to recover from a stuck/lingering session id after a mid-call failure. "
				"Stateless: no session_id required. Returns {sessions:[{session_id, asset_path, "
				"created_time, last_access_time, timeout_minutes}, ...]}.");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_ListActiveSessions::GetInputSchema() const
{
	FToolSchemaBuilder S;
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_ListActiveSessions::Execute(const TSharedPtr<FJsonObject>& /*Arguments*/)
{
	// MetaSound + SoundCue share the "audio_edit" lock-name; this enumeration is the audio-edit
	// view. Each session's AssetPath disambiguates between cohorts at the caller layer.
	const TArray<FMCPSession> Sessions = FClaireonSessionManager::Get().ListSessions(TEXT("audio_edit"));

	TArray<TSharedPtr<FJsonValue>> SessionsArr;
	for (const FMCPSession& Sess : Sessions)
	{
		TSharedPtr<FJsonObject> SJ = MakeShared<FJsonObject>();
		SJ->SetStringField(TEXT("session_id"), Sess.SessionId);
		SJ->SetStringField(TEXT("asset_path"), Sess.AssetPath);
		SJ->SetStringField(TEXT("created_time"), Sess.CreatedTime.ToIso8601());
		SJ->SetStringField(TEXT("last_access_time"), Sess.LastAccessTime.ToIso8601());
		SJ->SetNumberField(TEXT("timeout_minutes"), Sess.TimeoutMinutes);
		SessionsArr.Add(MakeShared<FJsonValueObject>(SJ));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetArrayField(TEXT("sessions"), SessionsArr);
	return MakeSuccessResult(Out, FString::Printf(TEXT("%d active MetaSound session(s)"), SessionsArr.Num()));
}
