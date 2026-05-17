// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_AddPossessable.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "ClaireonSequenceEditHandlers.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_AddPossessable::GetOperation() const { return TEXT("add_possessable"); }

FString ClaireonLevelSequenceTool_AddPossessable::GetDescription() const
{
	return TEXT("Add a possessable binding (a reference to an existing world actor) to the Level Sequence. Focuses the new binding.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_AddPossessable::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddString(TEXT("label"), TEXT("Display label for the new binding."), true);
	Builder.AddString(TEXT("object_class"), TEXT("Full class path (e.g. /Script/Engine.CameraActor)."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_AddPossessable::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	FString Label;
	if (!Arguments->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: label"));
	}
	FString ObjectClassPath;
	if (!Arguments->TryGetStringField(TEXT("object_class"), ObjectClassPath) || ObjectClassPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: object_class"));
	}
	UClass* ObjectClass = FindObject<UClass>(nullptr, *ObjectClassPath);
	if (!ObjectClass)
	{
		ObjectClass = LoadObject<UClass>(nullptr, *ObjectClassPath);
	}
	if (!ObjectClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not resolve object_class: %s"), *ObjectClassPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Possessable")));
	FMovieSceneBinding AddedBinding;
	if (!Claireon::SequenceEdit::ApplyAddPossessable(Data->Sequence.Get(), FName(*Label), ObjectClass, AddedBinding, Error))
	{
		return MakeErrorResult(Error);
	}
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	UMovieScene* MovieScene = Data->Sequence->GetMovieScene();
	const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
	int32 NewIndex = INDEX_NONE;
	for (int32 i = 0; i < Bindings.Num(); ++i)
	{
		if (Bindings[i].GetObjectGuid() == AddedBinding.GetObjectGuid())
		{
			NewIndex = i;
			break;
		}
	}
	Data->PushHistory();
	Data->FocusedBindingIndex = NewIndex;
	Data->FocusedTrackIndex = INDEX_NONE;
	Data->LastOperationStatus = FString::Printf(TEXT("Added possessable '%s' (%s)"),
		*Label, *ObjectClass->GetName());
	return BuildStateResponse(SessionId, Data);
}
