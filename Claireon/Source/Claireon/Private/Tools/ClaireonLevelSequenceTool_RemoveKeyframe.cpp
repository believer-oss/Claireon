// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_RemoveKeyframe.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "ClaireonSequenceEditHandlers.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneSection.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_RemoveKeyframe::GetOperation() const { return TEXT("remove_keyframe"); }

FString ClaireonLevelSequenceTool_RemoveKeyframe::GetDescription() const
{
	return TEXT("Remove all keyframes at a given frame from the focused section.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_RemoveKeyframe::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddInteger(TEXT("frame"), TEXT("Frame number to remove keys from."), true);
	Builder.AddInteger(TEXT("section_index"), TEXT("Optional explicit section index (defaults to first section on focused track)."));
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_RemoveKeyframe::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	int32 Frame = 0;
	if (!Arguments->TryGetNumberField(TEXT("frame"), Frame))
	{
		return MakeErrorResult(TEXT("Missing required parameter: frame"));
	}
	int32 SectionIndex = -1;
	int32 SecVal = 0;
	if (Arguments->TryGetNumberField(TEXT("section_index"), SecVal))
	{
		SectionIndex = SecVal;
	}

	UMovieSceneSection* Section = ClaireonLevelSequenceInternal::ResolveFocusedSection(Data, SectionIndex, Error);
	if (!Section)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Keyframe")));
	if (!ApplyRemoveKeyframe(Section, FFrameNumber(Frame), Error))
	{
		return MakeErrorResult(Error);
	}
	Section->MarkAsChanged();
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Removed keyframe(s) at frame %d"), Frame);
	return BuildStateResponse(SessionId, Data);
}
