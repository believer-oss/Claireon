// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_RemovePossessable.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "ClaireonSequenceEditHandlers.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_RemovePossessable::GetOperation() const { return TEXT("remove_possessable"); }

FString ClaireonLevelSequenceTool_RemovePossessable::GetDescription() const
{
	return TEXT("Remove a binding (possessable or spawnable) from the Level Sequence by label or guid.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_RemovePossessable::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddString(TEXT("label"), TEXT("Binding label to remove (provide this OR guid)."));
	Builder.AddString(TEXT("guid"), TEXT("Binding GUID to remove (provide this OR label)."));
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_RemovePossessable::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	UMovieScene* MovieScene = Data->Sequence->GetMovieScene();
	FString Label, GuidStr;
	Arguments->TryGetStringField(TEXT("label"), Label);
	Arguments->TryGetStringField(TEXT("guid"), GuidStr);

	int32 Index = INDEX_NONE;
	FGuid Guid;
	if (!ClaireonLevelSequenceInternal::FindBindingByLabelOrGuid(MovieScene, Label, GuidStr, Index, Guid, Error))
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Possessable")));
	if (!Claireon::SequenceEdit::ApplyRemovePossessable(Data->Sequence.Get(), Guid, Error))
	{
		return MakeErrorResult(Error);
	}
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	if (Data->FocusedBindingIndex == Index)
	{
		Data->FocusedBindingIndex = INDEX_NONE;
		Data->FocusedTrackIndex = INDEX_NONE;
	}
	Data->LastOperationStatus = FString::Printf(TEXT("Removed binding %s"),
		*Guid.ToString(EGuidFormats::DigitsWithHyphens));
	return BuildStateResponse(SessionId, Data);
}
