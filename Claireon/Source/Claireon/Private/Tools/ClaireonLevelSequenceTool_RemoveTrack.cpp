// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_RemoveTrack.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "ClaireonSequenceEditHandlers.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_RemoveTrack::GetOperation() const { return TEXT("remove_track"); }

FString ClaireonLevelSequenceTool_RemoveTrack::GetDescription() const
{
    return TEXT("Remove a track (by index) from the focused binding (or root tracks when no binding is focused). Session-mode tool: open via level_sequence_open first.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_RemoveTrack::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddInteger(TEXT("track_index"), TEXT("Track index to remove."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_RemoveTrack::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FSequenceEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}
	if (!Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}
	int32 TrackIndex = -1;
	int32 NumVal = 0;
	if (Arguments->TryGetNumberField(TEXT("track_index"), NumVal))
	{
		TrackIndex = NumVal;
	}
	else
	{
		return MakeErrorResult(TEXT("Missing required parameter: track_index"));
	}

	UMovieScene* MovieScene = Data->Sequence->GetMovieScene();
	FGuid BindingGuid;
	if (Data->FocusedBindingIndex != INDEX_NONE && Data->FocusedBindingIndex < MovieScene->GetBindings().Num())
	{
		BindingGuid = MovieScene->GetBindings()[Data->FocusedBindingIndex].GetObjectGuid();
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Track")));
	if (!Claireon::SequenceEdit::ApplyRemoveTrack(Data->Sequence.Get(), BindingGuid, TrackIndex, Error))
	{
		return MakeErrorResult(Error);
	}
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	if (Data->FocusedTrackIndex == TrackIndex)
	{
		Data->FocusedTrackIndex = INDEX_NONE;
	}
	Data->LastOperationStatus = FString::Printf(TEXT("Removed track [%d]"), TrackIndex);
	return BuildStateResponse(SessionId, Data);
}
