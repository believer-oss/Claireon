// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_SetPlaybackRange.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "ScopedTransaction.h"
#include "Misc/FrameRate.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_SetPlaybackRange::GetOperation() const { return TEXT("set_playback_range"); }

FString ClaireonLevelSequenceTool_SetPlaybackRange::GetDescription() const
{
    // start_frame/end_frame are DISPLAY-RATE frames by default (matches the UI / Sequencer
    // playhead numbers). Pass frame_units="tick" for raw MovieScene tick units (TickResolution).
    return TEXT("Set the sequence's playback range. By default, start_frame and end_frame are "
                "DISPLAY-RATE frames (the numbers shown in the Sequencer UI). Pass "
                "frame_units=\"tick\" to interpret them as raw tick frames (TickResolution units). "
                "start_frame inclusive, end_frame exclusive. Session-mode tool: open via "
                "level_sequence_open first.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_SetPlaybackRange::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddInteger(TEXT("start_frame"), TEXT("Playback range start (inclusive). Display-rate frames unless frame_units=\"tick\"."), true);
	Builder.AddInteger(TEXT("end_frame"), TEXT("Playback range end (exclusive). Display-rate frames unless frame_units=\"tick\"."), true);
	Builder.AddString(TEXT("frame_units"), TEXT("Interpretation of start_frame/end_frame. \"display\" (default) = display-rate frames; \"tick\" = raw tick frames."));
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_SetPlaybackRange::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	int32 StartFrame = 0, EndFrame = 0;
	if (!Arguments->TryGetNumberField(TEXT("start_frame"), StartFrame))
	{
		return MakeErrorResult(TEXT("Missing required parameter: start_frame"));
	}
	if (!Arguments->TryGetNumberField(TEXT("end_frame"), EndFrame))
	{
		return MakeErrorResult(TEXT("Missing required parameter: end_frame"));
	}
	if (EndFrame <= StartFrame)
	{
		return MakeErrorResult(TEXT("end_frame must be greater than start_frame"));
	}

	// Default frame_units is "display" (display-rate frames). Caller can opt into raw
	// tick units. MovieScene stores playback range in tick units internally, so we always
	// convert to ticks before calling SetPlaybackRange.
	FString FrameUnits = TEXT("display");
	Arguments->TryGetStringField(TEXT("frame_units"), FrameUnits);
	const bool bDisplayUnits = !FrameUnits.Equals(TEXT("tick"), ESearchCase::IgnoreCase);

	UMovieScene* MovieScene = Data->Sequence->GetMovieScene();
	int32 StartTick = StartFrame;
	int32 EndTick = EndFrame;
	if (bDisplayUnits)
	{
		const FFrameRate DisplayRate = MovieScene->GetDisplayRate();
		const FFrameRate TickResolution = MovieScene->GetTickResolution();
		// Convert display-rate frame numbers into tick frame numbers via FFrameRate.
		StartTick = FFrameRate::TransformTime(FFrameTime(FFrameNumber(StartFrame)), DisplayRate, TickResolution).FrameNumber.Value;
		EndTick   = FFrameRate::TransformTime(FFrameTime(FFrameNumber(EndFrame)),   DisplayRate, TickResolution).FrameNumber.Value;
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Playback Range")));
	MovieScene->SetPlaybackRange(FFrameNumber(StartTick), EndTick - StartTick);
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Playback range set to [%d, %d) (%s)"),
		StartFrame, EndFrame, bDisplayUnits ? TEXT("display-rate") : TEXT("tick"));
	return BuildStateResponse(SessionId, Data);
}
