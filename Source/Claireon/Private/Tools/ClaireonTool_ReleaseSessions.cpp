// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ReleaseSessions.h"
#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_ReleaseSessions::GetCategory() const { return TEXT("session"); }
FString ClaireonTool_ReleaseSessions::GetOperation() const { return TEXT("release"); }

FString ClaireonTool_ReleaseSessions::GetDescription() const
{
	return TEXT("Close (force-release) Claireon MCP editing sessions: one session by session_id, the session "
				"holding a given asset_path, or every open session with force_all=true. No dirty-state guard -- "
				"release always succeeds and unsaved in-session edits are lost. Use it to clear the blocked-session "
				"error a stale open session causes.");
}

TSharedPtr<FJsonObject> ClaireonTool_ReleaseSessions::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// session_id (optional) -- the id the blocked-session recovery hints name.
	TSharedPtr<FJsonObject> SessionIdProp = MakeShared<FJsonObject>();
	SessionIdProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionIdProp->SetStringField(TEXT("description"),
		TEXT("Session id to release, as reported by session_list or by a blocked-session error. "
			 "Takes precedence over asset_path."));
	Properties->SetObjectField(TEXT("session_id"), SessionIdProp);

	// asset_path (optional)
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"),
		TEXT("Asset path to release the session for (e.g. '/Game/Art/NS_MySystem'). "
			 "Releases the single session holding a lock on this asset."));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// force_all (optional)
	TSharedPtr<FJsonObject> ForceAllProp = MakeShared<FJsonObject>();
	ForceAllProp->SetStringField(TEXT("type"), TEXT("boolean"));
	ForceAllProp->SetStringField(TEXT("description"),
		TEXT("If true, release ALL active sessions. Overrides asset_path. Default: false."));
	Properties->SetObjectField(TEXT("force_all"), ForceAllProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_ReleaseSessions::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	const bool bForceAll = Arguments->GetBoolField(TEXT("force_all"));

	if (bForceAll)
	{
		const int32 Count = FClaireonSessionManager::Get().ForceReleaseAll();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("released_count"), Count);

		return MakeSuccessResult(Data,
			FString::Printf(TEXT("Force-released all sessions (%d released)"), Count));
	}

	// session_id outranks asset_path: it is what the blocked-session recovery hints
	// hand the caller, and it identifies exactly one session.
	const FString SessionId = Arguments->GetStringField(TEXT("session_id"));
	if (!SessionId.IsEmpty())
	{
		const bool bClosed = FClaireonSessionManager::Get().CloseSession(SessionId);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("released_count"), bClosed ? 1 : 0);
		Data->SetStringField(TEXT("session_id"), SessionId);

		if (bClosed)
		{
			return MakeSuccessResult(Data,
				FString::Printf(TEXT("Released session '%s'"), *SessionId));
		}
		return MakeSuccessResult(Data,
			FString::Printf(TEXT("No active session found for session_id '%s'"), *SessionId));
	}

	const FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
	if (AssetPath.IsEmpty())
	{
		// Stage 070 residual (N3b): erroring on a bare call is correct -- releasing
		// something unspecified would be worse. But the caller is here BECAUSE a
		// session is blocking them, and the tool knows exactly which. Naming the
		// parameters and making the caller run session_list to find the argument is
		// a round trip for information already in hand.
		FString Message = TEXT("One of 'session_id', 'asset_path', or 'force_all=true' is required.");

		const TArray<FMCPSession> Open = FClaireonSessionManager::Get().ListSessions();
		if (Open.Num() == 0)
		{
			Message += TEXT(" No sessions are currently open, so there is nothing to release.");
		}
		else
		{
			Message += FString::Printf(TEXT(" %d session%s currently open:"),
				Open.Num(), Open.Num() == 1 ? TEXT(" is") : TEXT("s are"));
			for (const FMCPSession& Session : Open)
			{
				const int32 IdleMinutes =
					static_cast<int32>((FDateTime::UtcNow() - Session.LastAccessTime).GetTotalMinutes());
				Message += FString::Printf(
					TEXT("\n  session_id='%s' tool=%s asset=%s (idle %dm)"),
					*Session.SessionId,
					*Session.ToolName,
					Session.AssetPath.IsEmpty() ? TEXT("<none>") : *Session.AssetPath,
					IdleMinutes);
			}
		}

		FToolResult Result = MakeErrorResult(Message);

		// Exactly one open session means there is exactly one right answer, so hand
		// back a directly callable arg set. Error-derived, so not latched.
		if (Open.Num() == 1)
		{
			TSharedPtr<FJsonObject> HintArgs = CloneHintArgs(Arguments);
			HintArgs->SetStringField(TEXT("session_id"), Open[0].SessionId);
			Result.Hint = MakeGuidanceHint(
				GetName(),
				FString::Printf(
					TEXT("One session is open ('%s' on %s); re-issue with that session_id to release it."),
					*Open[0].SessionId,
					Open[0].AssetPath.IsEmpty() ? TEXT("<none>") : *Open[0].AssetPath),
				HintArgs);
		}
		return Result;
	}

	const int32 Count = FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("released_count"), Count);
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	if (Count > 0)
	{
		return MakeSuccessResult(Data,
			FString::Printf(TEXT("Released session for asset '%s'"), *AssetPath));
	}

	return MakeSuccessResult(Data,
		FString::Printf(TEXT("No active session found for asset '%s'"), *AssetPath));
}