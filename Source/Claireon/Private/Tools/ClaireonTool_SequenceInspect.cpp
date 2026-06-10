// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SequenceInspect.h"
#include "Tools/ClaireonSequenceHelpers.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneMarkedFrame.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTrack.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

FString ClaireonTool_SequenceInspect::GetCategory() const { return TEXT("level"); }
FString ClaireonTool_SequenceInspect::GetOperation() const { return TEXT("sequence_inspect"); }

FString ClaireonTool_SequenceInspect::GetDescription() const
{
	return TEXT("Inspect a Level Sequence asset. Dumps bindings (Possessable/Spawnable), "
				"tracks, sections, keyframes, marked frames, playback range, display rate "
				"and tick resolution. Stateless -- no session required.");
}

FString ClaireonTool_SequenceInspect::GetFullDescription() const
{
	return TEXT(
		"Inspect a Level Sequence asset.\n"
		"\n"
		"Returns a structured description of the sequence: its display/tick rates, playback "
		"range, marked frames, and every binding with the tracks, sections, and optionally "
		"keyframes under it. A human-readable dump is also returned in the `formatted` field.\n"
		"\n"
		"Example input:\n"
		"  {\n"
		"    \"asset_path\": \"/Game/Cinematics/LS_Intro.LS_Intro\",\n"
		"    \"include_keyframes\": true,\n"
		"    \"include_sections\": true\n"
		"  }\n"
		"\n"
		"Example output shape (success):\n"
		"  {\n"
		"    \"asset_path\": \"...\",\n"
		"    \"display_rate\": \"30000/1000\",\n"
		"    \"tick_resolution\": \"24000/1\",\n"
		"    \"playback_range\": { \"start_frame\": 0, \"end_frame\": 7200, ... },\n"
		"    \"bindings\": [ { \"name\": \"...\", \"guid\": \"...\", \"kind\": \"Possessable\", \"tracks\": [...] } ],\n"
		"    \"marked_frames\": [ { \"frame\": 100, \"label\": \"cut\" } ],\n"
		"    \"formatted\": \"=== Level Sequence: ... ===\\n...\"\n"
		"  }\n"
		"\n"
		"On failure returns a structured error with the offending asset_path.");
}

TSharedPtr<FJsonObject> ClaireonTool_SequenceInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"),
		TEXT("Unreal asset path to the Level Sequence (e.g. /Game/Cinematics/LS_Intro.LS_Intro)."));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	TSharedPtr<FJsonObject> IncludeKeyframesProp = MakeShared<FJsonObject>();
	IncludeKeyframesProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeKeyframesProp->SetStringField(TEXT("description"),
		TEXT("If true (default), emit per-section keyframe times and event endpoint names."));
	Properties->SetObjectField(TEXT("include_keyframes"), IncludeKeyframesProp);

	TSharedPtr<FJsonObject> IncludeSectionsProp = MakeShared<FJsonObject>();
	IncludeSectionsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeSectionsProp->SetStringField(TEXT("description"),
		TEXT("If true (default), emit per-track section detail (range, row, easing)."));
	Properties->SetObjectField(TEXT("include_sections"), IncludeSectionsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_SequenceInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments object"));
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	bool bIncludeKeyframes = true;
	Arguments->TryGetBoolField(TEXT("include_keyframes"), bIncludeKeyframes);

	bool bIncludeSections = true;
	Arguments->TryGetBoolField(TEXT("include_sections"), bIncludeSections);

	FString Error;
	ULevelSequence* Sequence = FClaireonSequenceHelpers::LoadLevelSequenceAsset(AssetPath, Error);
	if (!Sequence)
	{
		return MakeErrorResult(Error);
	}

	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Level Sequence %s has no MovieScene"), *AssetPath));
	}

	const FString Formatted = FClaireonSequenceHelpers::FormatSequenceStructure(
		Sequence, bIncludeKeyframes, bIncludeSections);

	// Structured output
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);

	const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
	const FFrameRate TickResolution = MovieScene->GetTickResolution();
	Data->SetStringField(TEXT("display_rate"),
		FString::Printf(TEXT("%d/%d"), DisplayRate.Numerator, DisplayRate.Denominator));
	Data->SetStringField(TEXT("tick_resolution"),
		FString::Printf(TEXT("%d/%d"), TickResolution.Numerator, TickResolution.Denominator));

	// Playback range
	TSharedPtr<FJsonObject> PBObj = MakeShared<FJsonObject>();
	const TRange<FFrameNumber> PlaybackRange = MovieScene->GetPlaybackRange();
	if (PlaybackRange.HasLowerBound())
	{
		const FFrameNumber StartFrame = PlaybackRange.GetLowerBoundValue();
		PBObj->SetNumberField(TEXT("start_frame"), StartFrame.Value);
		PBObj->SetNumberField(TEXT("start_seconds"), TickResolution.AsSeconds(FFrameTime(StartFrame)));
	}
	if (PlaybackRange.HasUpperBound())
	{
		const FFrameNumber EndFrame = PlaybackRange.GetUpperBoundValue();
		PBObj->SetNumberField(TEXT("end_frame"), EndFrame.Value);
		PBObj->SetNumberField(TEXT("end_seconds"), TickResolution.AsSeconds(FFrameTime(EndFrame)));
	}
	Data->SetObjectField(TEXT("playback_range"), PBObj);

	// Marked frames
	TArray<TSharedPtr<FJsonValue>> MarkedArr;
	for (const FMovieSceneMarkedFrame& Mark : MovieScene->GetMarkedFrames())
	{
		TSharedPtr<FJsonObject> MarkObj = MakeShared<FJsonObject>();
		MarkObj->SetNumberField(TEXT("frame"), Mark.FrameNumber.Value);
		MarkObj->SetStringField(TEXT("label"), Mark.Label);
		MarkedArr.Add(MakeShared<FJsonValueObject>(MarkObj));
	}
	Data->SetArrayField(TEXT("marked_frames"), MarkedArr);

	// Bindings
	TArray<TSharedPtr<FJsonValue>> BindingsArr;
	const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
	for (const FMovieSceneBinding& Binding : Bindings)
	{
		TSharedPtr<FJsonObject> BObj = MakeShared<FJsonObject>();
		BObj->SetStringField(TEXT("name"), Binding.GetName());
		BObj->SetStringField(TEXT("guid"),
			Binding.GetObjectGuid().ToString(EGuidFormats::DigitsWithHyphens));

		FString Kind = TEXT("Binding");
		FString ClassName;
		if (FMovieScenePossessable* Poss = MovieScene->FindPossessable(Binding.GetObjectGuid()))
		{
			Kind = TEXT("Possessable");
			if (const UClass* Cls = Poss->GetPossessedObjectClass())
			{
				ClassName = Cls->GetName();
			}
		}
		else if (FMovieSceneSpawnable* Spawn = MovieScene->FindSpawnable(Binding.GetObjectGuid()))
		{
			Kind = TEXT("Spawnable");
			if (UObject* Template = Spawn->GetObjectTemplate())
			{
				ClassName = Template->GetClass()->GetName();
			}
		}
		BObj->SetStringField(TEXT("kind"), Kind);
		if (!ClassName.IsEmpty())
		{
			BObj->SetStringField(TEXT("class"), ClassName);
		}

		TArray<TSharedPtr<FJsonValue>> TracksArr;
		for (const UMovieSceneTrack* Track : Binding.GetTracks())
		{
			if (!Track)
			{
				continue;
			}
			TSharedPtr<FJsonObject> TObj = MakeShared<FJsonObject>();
			TObj->SetStringField(TEXT("type"), Track->GetClass()->GetName());
			TObj->SetStringField(TEXT("name"), Track->GetTrackName().ToString());
			TObj->SetStringField(TEXT("display_name"), Track->GetDisplayName().ToString());
			if (bIncludeSections)
			{
				TObj->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
			}
			TracksArr.Add(MakeShared<FJsonValueObject>(TObj));
		}
		BObj->SetArrayField(TEXT("tracks"), TracksArr);
		BindingsArr.Add(MakeShared<FJsonValueObject>(BObj));
	}
	Data->SetArrayField(TEXT("bindings"), BindingsArr);

	Data->SetStringField(TEXT("formatted"), Formatted);

	const FString AssetName = FPaths::GetBaseFilename(AssetPath);
	const FString Summary = FString::Printf(
		TEXT("%s: %d bindings, display rate %d/%d"),
		*AssetName, Bindings.Num(), DisplayRate.Numerator, DisplayRate.Denominator);

	FToolResult Result = MakeSuccessResult(Data, Summary);
	// Also surface the formatted dump as the plain-content string so MakeErrorResult
	// vs MakeSuccessResult behave consistently with the Niagara inspect tool which
	// returns the formatted text in the Summary.
	Result.Summary = Formatted;
	return Result;
}
