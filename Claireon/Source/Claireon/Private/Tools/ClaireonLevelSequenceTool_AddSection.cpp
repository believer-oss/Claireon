// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_AddSection.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "ClaireonSequenceEditHandlers.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_AddSection::GetOperation() const { return TEXT("add_section"); }

FString ClaireonLevelSequenceTool_AddSection::GetDescription() const
{
    return TEXT("Add a section (frame-range slice of animation data) to the focused track. Session-mode tool: open via level_sequence_open first.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_AddSection::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddInteger(TEXT("start_frame"), TEXT("Section start frame (inclusive)."), true);
	Builder.AddInteger(TEXT("end_frame"), TEXT("Section end frame (exclusive)."), true);
	Builder.AddInteger(TEXT("row_index"), TEXT("Optional row index for layered tracks."));
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_AddSection::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	int32 RowIndex = -1;
	int32 RowVal = 0;
	if (Arguments->TryGetNumberField(TEXT("row_index"), RowVal))
	{
		RowIndex = RowVal;
	}

	UMovieSceneTrack* Track = ClaireonLevelSequenceInternal::ResolveFocusedTrack(
		Data->Sequence->GetMovieScene(), Data->FocusedBindingIndex, Data->FocusedTrackIndex, Error);
	if (!Track)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Section")));
	UMovieSceneSection* NewSection = nullptr;
	if (!Claireon::SequenceEdit::ApplyAddSection(Track, FFrameNumber(StartFrame), FFrameNumber(EndFrame),
			RowIndex, NewSection, Error))
	{
		return MakeErrorResult(Error);
	}
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Added section [%d, %d)"), StartFrame, EndFrame);
	return BuildStateResponse(SessionId, Data);
}
