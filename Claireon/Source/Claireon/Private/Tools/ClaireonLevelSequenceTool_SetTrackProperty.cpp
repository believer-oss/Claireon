// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_SetTrackProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "UObject/UnrealType.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_SetTrackProperty::GetOperation() const { return TEXT("set_track_property"); }

FString ClaireonLevelSequenceTool_SetTrackProperty::GetDescription() const
{
	return TEXT("Set a UPROPERTY on a track by name. Uses ImportText for value coercion.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_SetTrackProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddInteger(TEXT("track_index"), TEXT("Track index (defaults to focused_track when omitted)."));
	Builder.AddString(TEXT("property_name"), TEXT("Name of the UPROPERTY to set."), true);
	Builder.AddString(TEXT("value"), TEXT("String value; ImportText coerces it to the property type."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_SetTrackProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	else if (Data->FocusedTrackIndex != INDEX_NONE)
	{
		TrackIndex = Data->FocusedTrackIndex;
	}
	else
	{
		return MakeErrorResult(TEXT("Missing required parameter: track_index (or focus_track first)"));
	}
	FString PropertyName, Value;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	UMovieScene* MovieScene = Data->Sequence->GetMovieScene();
	UMovieSceneTrack* Track = nullptr;
	if (Data->FocusedBindingIndex != INDEX_NONE && Data->FocusedBindingIndex < MovieScene->GetBindings().Num())
	{
		const TArray<UMovieSceneTrack*>& Tracks = MovieScene->GetBindings()[Data->FocusedBindingIndex].GetTracks();
		if (TrackIndex >= 0 && TrackIndex < Tracks.Num())
		{
			Track = Tracks[TrackIndex];
		}
	}
	else
	{
		const TArray<UMovieSceneTrack*>& Tracks = MovieScene->GetTracks();
		if (TrackIndex >= 0 && TrackIndex < Tracks.Num())
		{
			Track = Tracks[TrackIndex];
		}
	}
	if (!Track)
	{
		return MakeErrorResult(FString::Printf(TEXT("Track index %d not found"), TrackIndex));
	}

	FProperty* Prop = FindFProperty<FProperty>(Track->GetClass(), *PropertyName);
	if (!Prop)
	{
		return MakeErrorResult(FString::Printf(TEXT("Property '%s' not found on %s"),
			*PropertyName, *Track->GetClass()->GetName()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Track Property")));
	void* Container = Prop->ContainerPtrToValuePtr<void>(Track);
	const TCHAR* Import = Prop->ImportText_Direct(*Value, Container, Track, PPF_None);
	if (!Import)
	{
		return MakeErrorResult(FString::Printf(TEXT("ImportText failed for property '%s' value '%s'"),
			*PropertyName, *Value));
	}
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Set %s.%s = %s"),
		*Track->GetClass()->GetName(), *PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}
