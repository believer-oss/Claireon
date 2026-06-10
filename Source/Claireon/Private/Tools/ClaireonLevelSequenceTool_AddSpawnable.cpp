// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_AddSpawnable.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_AddSpawnable::GetOperation() const { return TEXT("add_spawnable"); }

FString ClaireonLevelSequenceTool_AddSpawnable::GetDescription() const
{
    return TEXT("Add a spawnable binding to the Level Sequence. Provide either a template_actor_path or an object_class. Session-mode tool: open via level_sequence_open first.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_AddSpawnable::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"), TEXT("Session identifier from open."), true);
	Builder.AddString(TEXT("label"), TEXT("Display label for the new spawnable."), true);
	Builder.AddString(TEXT("object_class"), TEXT("Class path used to construct a template object (use this OR template_actor_path)."));
	Builder.AddString(TEXT("template_actor_path"), TEXT("Path to an existing object to use as the spawnable template."));
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("If true, returns brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_AddSpawnable::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	FString ObjectClassPath, TemplateActorPath;
	Arguments->TryGetStringField(TEXT("object_class"), ObjectClassPath);
	Arguments->TryGetStringField(TEXT("template_actor_path"), TemplateActorPath);

	UObject* Template = nullptr;
	if (!TemplateActorPath.IsEmpty())
	{
		Template = LoadObject<UObject>(nullptr, *TemplateActorPath);
		if (!Template)
		{
			return MakeErrorResult(FString::Printf(TEXT("Could not load template: %s"), *TemplateActorPath));
		}
	}
	else if (!ObjectClassPath.IsEmpty())
	{
		UClass* Cls = FindObject<UClass>(nullptr, *ObjectClassPath);
		if (!Cls)
		{
			Cls = LoadObject<UClass>(nullptr, *ObjectClassPath);
		}
		if (!Cls)
		{
			return MakeErrorResult(FString::Printf(TEXT("Could not resolve object_class: %s"), *ObjectClassPath));
		}
		Template = NewObject<UObject>(Data->Sequence.Get(), Cls, NAME_None, RF_Transactional);
		if (!Template)
		{
			return MakeErrorResult(TEXT("Failed to instantiate template object for spawnable"));
		}
	}
	else
	{
		return MakeErrorResult(TEXT("add_spawnable requires 'template_actor_path' or 'object_class'"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Spawnable")));
	UMovieScene* MovieScene = Data->Sequence->GetMovieScene();
	const FGuid Guid = MovieScene->AddSpawnable(Label, *Template);
	if (!Guid.IsValid())
	{
		return MakeErrorResult(TEXT("AddSpawnable failed"));
	}
	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
	int32 NewIndex = INDEX_NONE;
	for (int32 i = 0; i < Bindings.Num(); ++i)
	{
		if (Bindings[i].GetObjectGuid() == Guid)
		{
			NewIndex = i;
			break;
		}
	}
	Data->PushHistory();
	Data->FocusedBindingIndex = NewIndex;
	Data->FocusedTrackIndex = INDEX_NONE;
	Data->LastOperationStatus = FString::Printf(TEXT("Added spawnable '%s'"), *Label);
	return BuildStateResponse(SessionId, Data);
}
