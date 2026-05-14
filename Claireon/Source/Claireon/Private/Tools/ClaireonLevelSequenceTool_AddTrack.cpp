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
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_AddTrack::GetOperation() const { return TEXT("sequence_add_track"); }

FString ClaireonLevelSequenceTool_AddTrack::GetDescription() const
{
	return TEXT("Add a track of the given type to the focused binding (or as a root track for event tracks).");
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
	const bool bIsEvent = TrackClass->IsChildOf(UMovieSceneEventTrack::StaticClass());
	if (!bIsEvent)
	{
		if (Data->FocusedBindingIndex == INDEX_NONE)
		{
			return MakeErrorResult(TEXT("add_track requires a focused binding for non-event tracks"));
		}
		const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
		if (Data->FocusedBindingIndex >= Bindings.Num())
		{
			return MakeErrorResult(TEXT("Focused binding index out of range"));
		}
		BindingGuid = Bindings[Data->FocusedBindingIndex].GetObjectGuid();
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Track")));
	UMovieSceneTrack* NewTrack = nullptr;
	if (!ApplyAddTrack(Data->Sequence.Get(), BindingGuid, TrackClass, NewTrack, Error))
	{
		return MakeErrorResult(Error);
	}
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Added %s track"), *TrackType);
	return BuildStateResponse(SessionId, Data);
}
