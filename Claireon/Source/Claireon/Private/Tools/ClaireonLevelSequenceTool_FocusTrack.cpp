// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_FocusTrack.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_FocusTrack::GetOperation() const { return TEXT("focus_track"); }

FString ClaireonLevelSequenceTool_FocusTrack::GetDescription() const
{
    return TEXT("Focus a track (by index) on the currently-focused binding. Subsequent section/keyframe ops target this track. Session-mode tool: open via level_sequence_open first.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_FocusTrack::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddInteger(TEXT("track_index"), TEXT("Index of the track on the focused binding."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_FocusTrack::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	if (Data->FocusedBindingIndex == INDEX_NONE)
	{
		return MakeErrorResult(TEXT("No focused binding; call focus_binding first"));
	}

	int32 TrackIndex = INDEX_NONE;
	int32 ParamValue = 0;
	if (Arguments->TryGetNumberField(TEXT("track_index"), ParamValue))
	{
		TrackIndex = ParamValue;
	}
	else
	{
		return MakeErrorResult(TEXT("Missing required parameter: track_index"));
	}

	UMovieScene* MovieScene = Data->Sequence->GetMovieScene();
	const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
	if (Data->FocusedBindingIndex < 0 || Data->FocusedBindingIndex >= Bindings.Num())
	{
		return MakeErrorResult(TEXT("Focused binding index out of range"));
	}
	const TArray<UMovieSceneTrack*>& Tracks = Bindings[Data->FocusedBindingIndex].GetTracks();
	if (TrackIndex < 0 || TrackIndex >= Tracks.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("track_index %d out of range (tracks=%d)"),
			TrackIndex, Tracks.Num()));
	}

	Data->PushHistory();
	Data->FocusedTrackIndex = TrackIndex;
	Data->LastOperationStatus = FString::Printf(TEXT("Focused track [%d]"), TrackIndex);
	return BuildStateResponse(SessionId, Data);
}
