// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_SetPlaybackRange.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_SetPlaybackRange::GetOperation() const { return TEXT("set_playback_range"); }

FString ClaireonLevelSequenceTool_SetPlaybackRange::GetDescription() const
{
	return TEXT("Set the sequence's playback range (start_frame inclusive, end_frame exclusive).");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_SetPlaybackRange::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddInteger(TEXT("start_frame"), TEXT("Playback range start (inclusive)."), true);
	Builder.AddInteger(TEXT("end_frame"), TEXT("Playback range end (exclusive)."), true);
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Playback Range")));
	UMovieScene* MovieScene = Data->Sequence->GetMovieScene();
	MovieScene->SetPlaybackRange(FFrameNumber(StartFrame), EndFrame - StartFrame);
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Playback range set to [%d, %d)"), StartFrame, EndFrame);
	return BuildStateResponse(SessionId, Data);
}
