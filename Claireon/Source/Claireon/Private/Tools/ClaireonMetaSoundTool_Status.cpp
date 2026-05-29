// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_Status.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Dom/JsonObject.h"

FString FClaireonMetaSoundTool_Status::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_Status::GetOperation() const { return TEXT("status"); }

FString FClaireonMetaSoundTool_Status::GetDescription() const
{
	return TEXT("Return the current state of a MetaSound editing session: asset path, dirty flag, "
				"input and output counts, and last operation status. Use to confirm session health or "
				"refresh the caller's view without making changes. Requires session_id.");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_Status::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by metasound_open"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_Status::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	FAudioEditToolData* Data = ClaireonAudioSessionRegistry::FindSession(SessionId, ESoundCohort::MetaSound);
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("MetaSound session not found: %s"), *SessionId));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetStringField(TEXT("asset_path"), Data->AssetPath);
	Out->SetStringField(TEXT("kind"), TEXT("metasound_source"));
	Out->SetBoolField(TEXT("dirty"), Data->bDirty);
	Out->SetStringField(TEXT("last_operation"), Data->LastOperationStatus);
	return MakeSuccessResult(Out, FString::Printf(TEXT("Session %s: %s"), *SessionId, *Data->LastOperationStatus));
}
