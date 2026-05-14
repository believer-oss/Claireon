// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundCueTool_Status.h"
#include "Tools/ClaireonAudioSessionRegistry.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundCue.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundCueTool_Status::GetCategory() const { return TEXT("soundcue"); }
FString FClaireonSoundCueTool_Status::GetOperation() const { return TEXT("status"); }

FString FClaireonSoundCueTool_Status::GetDescription() const
{
	return TEXT("Return current state of a SoundCue editing session (asset path, focused node, last operation).");
}

TSharedPtr<FJsonObject> FClaireonSoundCueTool_Status::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session id returned by soundcue_open"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundCueTool_Status::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	FAudioEditToolData* Data = ClaireonAudioSessionRegistry::FindSession(SessionId, ESoundCohort::SoundCue);
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("SoundCue session not found: %s"), *SessionId));
	}

	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("session_id"), SessionId);
	Out->SetStringField(TEXT("asset_path"), Data->AssetPath);
	Out->SetNumberField(TEXT("focused_node_index"), Data->FocusedNodeIndex);
	Out->SetBoolField(TEXT("dirty"), Data->bDirty);
	Out->SetStringField(TEXT("last_operation"), Data->LastOperationStatus);
	if (USoundCue* Cue = Cast<USoundCue>(Data->Asset.Get()))
	{
#if WITH_EDITORONLY_DATA
		Out->SetNumberField(TEXT("node_count"), Cue->AllNodes.Num());
#endif
	}
	return MakeSuccessResult(Out, FString::Printf(TEXT("Session %s: %s"), *SessionId, *Data->LastOperationStatus));
}
