// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_RemoveSection.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "ClaireonSequenceEditHandlers.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_RemoveSection::GetOperation() const { return TEXT("remove_section"); }

FString ClaireonLevelSequenceTool_RemoveSection::GetDescription() const
{
    return TEXT("Remove a section (by index) from the focused track of the Level Sequence. Session-mode tool: open via level_sequence_open first.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_RemoveSection::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddInteger(TEXT("section_index"), TEXT("Index of the section on the focused track."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_RemoveSection::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	int32 SectionIndex = -1;
	int32 NumVal = 0;
	if (Arguments->TryGetNumberField(TEXT("section_index"), NumVal))
	{
		SectionIndex = NumVal;
	}
	else
	{
		return MakeErrorResult(TEXT("Missing required parameter: section_index"));
	}

	UMovieSceneTrack* Track = ClaireonLevelSequenceInternal::ResolveFocusedTrack(
		Data->Sequence->GetMovieScene(), Data->FocusedBindingIndex, Data->FocusedTrackIndex, Error);
	if (!Track)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Section")));
	if (!Claireon::SequenceEdit::ApplyRemoveSection(Track, SectionIndex, Error))
	{
		return MakeErrorResult(Error);
	}
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Removed section [%d]"), SectionIndex);
	return BuildStateResponse(SessionId, Data);
}
