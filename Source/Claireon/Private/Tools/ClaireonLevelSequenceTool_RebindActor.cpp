// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_RebindActor.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "ClaireonSequenceEditHandlers.h"
#include "ClaireonPathResolver.h"
#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_RebindActor::GetOperation() const
{
	return TEXT("rebind_actor");
}

FString ClaireonLevelSequenceTool_RebindActor::GetDescription() const
{
	return TEXT("Re-attach a world AActor to an existing possessable binding "
	            "GUID without changing the GUID. Repairs bindings whose actor "
	            "reference is unresolved (e.g. World Partition cells unstreamed "
	            "at authoring time). Use clear=true to drop the binding's "
	            "actor refs while keeping the GUID.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_RebindActor::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("session_id"),
		TEXT("Session id from claireon.level_sequence_open."), /*bRequired=*/true);
	Builder.AddString(TEXT("label"),
		TEXT("Binding label to target. One of label/guid is required."));
	Builder.AddString(TEXT("guid"),
		TEXT("Binding GUID to target. Preferred when both label and guid are "
		     "present; if both disagree, returns an error."));
	Builder.AddString(TEXT("actor_path"),
		TEXT("World object path of the target actor (e.g. /Game/Maps/Foo.Foo:"
		     "PersistentLevel.CineCamera_0). One of actor_path/actor_label is "
		     "required unless clear=true."));
	Builder.AddString(TEXT("actor_label"),
		TEXT("AActor::GetActorLabel() to match in the editor world. One of "
		     "actor_path/actor_label is required unless clear=true."));
	Builder.AddBoolean(TEXT("clear"),
		TEXT("If true, clears the binding's actor refs and ignores actor_path/"
		     "actor_label."));
	Builder.AddBoolean(TEXT("suppress_output"),
		TEXT("Standard suppress flag; consumed by BuildStateResponse."));
	return Builder.Build();
}

TArray<FString> ClaireonLevelSequenceTool_RebindActor::GetSearchKeywords() const
{
	return { TEXT("rebind"), TEXT("bind"), TEXT("actor"),
	         TEXT("possessable"), TEXT("repair") };
}

FToolResult ClaireonLevelSequenceTool_RebindActor::Execute(
	const TSharedPtr<FJsonObject>& Arguments)
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

	FString Label, GuidStr, ActorPath, ActorLabel;
	Arguments->TryGetStringField(TEXT("label"), Label);
	Arguments->TryGetStringField(TEXT("guid"),  GuidStr);
	Arguments->TryGetStringField(TEXT("actor_path"),  ActorPath);
	Arguments->TryGetStringField(TEXT("actor_label"), ActorLabel);
	bool bClear = false;
	Arguments->TryGetBoolField(TEXT("clear"), bClear);

	int32 Index = INDEX_NONE;
	FGuid BindingGuid;
	if (!ClaireonLevelSequenceInternal::FindBindingByLabelOrGuid(
			Data->Sequence->GetMovieScene(), Label, GuidStr, Index, BindingGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("editor world unavailable"));
	}
	UWorld* EditorWorld = GEditor->GetEditorWorldContext().World();
	if (!EditorWorld)
	{
		return MakeErrorResult(TEXT("editor world unavailable"));
	}

	AActor* Actor = nullptr;
	FString ActorPathOrLabel;
	if (!bClear)
	{
		if (!ActorPath.IsEmpty())
		{
			ClaireonPathResolver::FResolveResult Resolved = ClaireonPathResolver::Resolve(ActorPath);
			if (!Resolved.bSuccess)
			{
				return MakeErrorResult(Resolved.Error);
			}
			Actor = FindObject<AActor>(nullptr, *Resolved.ResolvedPath.Path);
			if (!Actor)
			{
				Actor = LoadObject<AActor>(nullptr, *Resolved.ResolvedPath.Path);
			}
			if (!Actor)
			{
				return MakeErrorResult(FString::Printf(
					TEXT("actor not found at path '%s'"), *Resolved.ResolvedPath.Path));
			}
			ActorPathOrLabel = Resolved.ResolvedPath.Path;
		}
		else if (!ActorLabel.IsEmpty())
		{
			TArray<AActor*> Hits;
			for (TActorIterator<AActor> It(EditorWorld); It; ++It)
			{
				if (It->GetActorLabel() == ActorLabel)
				{
					Hits.Add(*It);
				}
			}
			if (Hits.Num() == 0)
			{
				return MakeErrorResult(FString::Printf(
					TEXT("no actor with label '%s' in editor world"), *ActorLabel));
			}
			if (Hits.Num() > 1)
			{
				return MakeErrorResult(FString::Printf(
					TEXT("ambiguous actor_label '%s' (matched %d actors)"),
					*ActorLabel, Hits.Num()));
			}
			Actor = Hits[0];
			ActorPathOrLabel = ActorLabel;
		}
		else
		{
			return MakeErrorResult(
				TEXT("must provide actor_path, actor_label, or clear=true"));
		}
	}

	FScopedTransaction Transaction(FText::FromString(
		TEXT("[Claireon] Rebind Possessable Actor")));

	if (!Claireon::SequenceEdit::ApplyRebindActor(Data->Sequence.Get(), BindingGuid, Actor, bClear, Error))
	{
		Transaction.Cancel();
		return MakeErrorResult(Error);
	}

	ClaireonLevelSequenceInternal::MarkMutated(Data->Sequence.Get());

	if (bClear)
	{
		Data->LastOperationStatus = FString::Printf(TEXT("Cleared binding %s"),
			*BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
	}
	else
	{
		Data->LastOperationStatus = FString::Printf(TEXT("Rebound binding %s -> %s"),
			*BindingGuid.ToString(EGuidFormats::DigitsWithHyphens),
			*ActorPathOrLabel);
	}

	return BuildStateResponse(SessionId, Data);
}
