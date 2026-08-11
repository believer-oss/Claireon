// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_AddEventKey.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "LevelSequence.h"
#include "Engine/Blueprint.h"
#include "MovieSceneSection.h"
#include "Sections/MovieSceneEventSectionBase.h"
#include "Channels/MovieSceneEvent.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_AddEventKey::GetOperation() const { return TEXT("add_event_key"); }

FString ClaireonLevelSequenceTool_AddEventKey::GetDescription() const
{
	return TEXT("Bind a free event-section entry point at the given frame to a Director Blueprint endpoint "
				"function named by endpoint_name. The endpoint must already exist -- create it first with "
				"level_sequence_create_event_endpoint. Session-mode tool: open via level_sequence_open first.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_AddEventKey::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddInteger(TEXT("frame"), TEXT("Frame number for the event key."), true);
	Builder.AddString(TEXT("endpoint_name"), TEXT("Name of the Director Blueprint endpoint function to bind."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_AddEventKey::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	FString EndpointName;
	if (!Arguments->TryGetStringField(TEXT("endpoint_name"), EndpointName) || EndpointName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: endpoint_name"));
	}

	ULevelSequence* Sequence = Data->Sequence.Get();
	UBlueprint* DirectorBP = IsValid(Sequence) ? Sequence->GetDirectorBlueprint() : nullptr;
	UClass* DirectorClass = IsValid(DirectorBP) ? Cast<UClass>(DirectorBP->GeneratedClass) : nullptr;
	UFunction* EndpointFunc = IsValid(DirectorClass) ? DirectorClass->FindFunctionByName(FName(*EndpointName)) : nullptr;
	if (!IsValid(EndpointFunc))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Event endpoint '%s' not found on Director class. Create it first (level_sequence_create_event_endpoint)."),
			*EndpointName));
	}

	UMovieSceneSection* Section = ClaireonLevelSequenceInternal::ResolveFocusedSection(Data, -1, Error);
	if (!IsValid(Section))
	{
		return MakeErrorResult(Error);
	}
	UMovieSceneEventSectionBase* EventSection = Cast<UMovieSceneEventSectionBase>(Section);
	if (!IsValid(EventSection))
	{
		return MakeErrorResult(TEXT("focused section is not an event section"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Event Key")));

	TArrayView<FMovieSceneEvent> Entries = EventSection->GetAllEntryPoints();
	bool bBound = false;
	for (FMovieSceneEvent& Entry : Entries)
	{
		if (!Entry.Ptrs.Function.Get())
		{
			Entry.Ptrs.Function = EndpointFunc;
			bBound = true;
			break;
		}
	}
	if (!bBound)
	{
		return MakeErrorResult(TEXT("no free entry point slot on event section; add more keys via the Sequencer first"));
	}
	Section->MarkAsChanged();
	ClaireonLevelSequenceInternal::MarkMutated(Sequence);

	Data->LastOperationStatus = FString::Printf(TEXT("Bound event key to endpoint '%s' at frame %d"),
		*EndpointName, Frame);
	return BuildStateResponse(SessionId, Data);
}
