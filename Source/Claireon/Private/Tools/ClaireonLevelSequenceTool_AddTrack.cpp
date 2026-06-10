// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_AddTrack.h"
#include "Tools/ClaireonSequenceHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "ClaireonSequenceEditHandlers.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "Tracks/MovieSceneEventTrack.h"
#include "Tracks/MovieSceneCameraCutTrack.h"
#include "Tracks/MovieSceneAudioTrack.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_AddTrack::GetOperation() const { return TEXT("add_track"); }

FString ClaireonLevelSequenceTool_AddTrack::GetDescription() const
{
    // Root-context tracks (event, camera_cut, audio) are added to UMovieScene
    // directly -- no focused binding required. Binding-context tracks
    // (transform, visibility, float, ...) require a focused binding. Mirrors
    // `binding_context` from level_sequence_list_track_types.
    // The new track is NOT focused automatically; call focus_track to make
    // subsequent section/keyframe ops target it.
    return TEXT("Add a track of the given type. Root-context tracks (event, camera_cut, audio) "
                "are added at sequence root and do not require a focused binding; "
                "binding-context tracks (transform, visibility, float, color, margin, "
                "2d_transform, widget_material) require focus_binding first. See "
                "level_sequence_list_track_types for the binding_context of each type. "
                "Note: the new track is NOT auto-focused -- call focus_track to make "
                "subsequent section/keyframe ops target it. "
                "Session-mode tool: open via level_sequence_open first.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_AddTrack::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddString(TEXT("track_type"), TEXT("Track type (e.g. transform, visibility, event, float). See level_sequence_list_track_types."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_AddTrack::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	FString TrackType;
	if (!Arguments->TryGetStringField(TEXT("track_type"), TrackType) || TrackType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: track_type"));
	}
	UClass* TrackClass = FClaireonSequenceHelpers::ResolveTrackClass(TrackType);
	if (!TrackClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown track_type: %s"), *TrackType));
	}

	UMovieScene* MovieScene = Data->Sequence->GetMovieScene();
	FGuid BindingGuid;
	// Root-context tracks: classes that live on UMovieScene (no binding parent).
	// Mirrors the `binding_context = "root"` / `"possessable_or_root"` rows in
	// level_sequence_list_track_types. Audio is `possessable_or_root` -- if a binding
	// is focused, attach to it; otherwise add at root. Event/camera_cut are root-only.
	const bool bIsRootOnly =
		TrackClass->IsChildOf(UMovieSceneEventTrack::StaticClass()) ||
		TrackClass->IsChildOf(UMovieSceneCameraCutTrack::StaticClass());
	const bool bIsRootOrPossessable =
		TrackClass->IsChildOf(UMovieSceneAudioTrack::StaticClass());
	const bool bRequiresBinding = !bIsRootOnly && !bIsRootOrPossessable;
	if (bRequiresBinding)
	{
		if (Data->FocusedBindingIndex == INDEX_NONE)
		{
			return MakeErrorResult(TEXT("add_track requires a focused binding for binding-context tracks; "
				"call focus_binding first or use a root-context track (event, camera_cut, audio)"));
		}
		const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
		if (Data->FocusedBindingIndex >= Bindings.Num())
		{
			return MakeErrorResult(TEXT("Focused binding index out of range"));
		}
		BindingGuid = Bindings[Data->FocusedBindingIndex].GetObjectGuid();
	}
	else if (bIsRootOrPossessable && Data->FocusedBindingIndex != INDEX_NONE)
	{
		// Audio: attach to focused binding when one is set.
		const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
		if (Data->FocusedBindingIndex < Bindings.Num())
		{
			BindingGuid = Bindings[Data->FocusedBindingIndex].GetObjectGuid();
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Track")));
	UMovieSceneTrack* NewTrack = nullptr;
	if (!Claireon::SequenceEdit::ApplyAddTrack(Data->Sequence.Get(), BindingGuid, TrackClass, NewTrack, Error))
	{
		return MakeErrorResult(Error);
	}
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Added %s track"), *TrackType);
	return BuildStateResponse(SessionId, Data);
}
