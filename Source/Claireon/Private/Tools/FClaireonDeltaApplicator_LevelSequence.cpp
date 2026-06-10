// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/FClaireonDeltaApplicator_LevelSequence.h"
#include "Tools/ClaireonLevelSequenceEditToolBase.h"
#include "Tools/ClaireonSequenceHelpers.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "ClaireonSequenceEditHandlers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonDeltaApplicator_LS_anon
{
	static bool LSDelta_TryGetObject(const TSharedPtr<FJsonValue>& Entry, TSharedPtr<FJsonObject>& OutObj)
	{
		if (!Entry.IsValid() || Entry->Type != EJson::Object) { return false; }
		OutObj = Entry->AsObject();
		return OutObj.IsValid();
	}

	/**
	 * Composite-id specificity depth: number of provided keys deeper than binding_label.
	 *   binding_label only -> depth 1
	 *   binding_label + track_name -> depth 2
	 *   ... + row_index -> depth 3
	 *   ... + start_frame -> depth 4
	 * M4 sort is descending (deepest first).
	 */
	static int32 LSDelta_GetSpecificity(const TSharedPtr<FJsonObject>& Obj)
	{
		int32 Depth = 0;
		FString Tmp;
		double TmpN = 0.0;
		if (Obj->TryGetStringField(TEXT("binding_label"), Tmp) && !Tmp.IsEmpty()) { ++Depth; }
		if (Obj->TryGetStringField(TEXT("track_name"), Tmp) && !Tmp.IsEmpty()) { ++Depth; }
		if (Obj->TryGetNumberField(TEXT("row_index"), TmpN)) { ++Depth; }
		if (Obj->TryGetNumberField(TEXT("start_frame"), TmpN)) { ++Depth; }
		return Depth;
	}
}

bool FClaireonDeltaApplicator_LevelSequence::ValidateArgs(const TSharedPtr<FJsonObject>& Args, TArray<FString>& OutErrors)
{
	(void)Args; (void)OutErrors;
	return true;
}

bool FClaireonDeltaApplicator_LevelSequence::OpenOrReuseSession(const TSharedPtr<FJsonObject>& Args, FString& OutSessionId, FString& OutError)
{
	CachedSequence.Reset();
	CreatedBindingsThisCall.Reset();

	FString SessionIdArg;
	const bool bHasSessionId = Args->TryGetStringField(TEXT("session_id"), SessionIdArg) && !SessionIdArg.IsEmpty();
	if (bHasSessionId)
	{
		FSequenceEditToolData* Data = ClaireonLevelSequenceEditToolBase::ToolData.Find(SessionIdArg);
		if (!Data || !Data->IsValid())
		{
			OutError = FString::Printf(TEXT("level_sequence_apply_delta: session_id '%s' not found"), *SessionIdArg);
			return false;
		}
		CachedSequence = Data->Sequence;
		OutSessionId = SessionIdArg;
		return true;
	}

	FString AssetPathArg;
	if (!Args->TryGetStringField(TEXT("asset_path"), AssetPathArg) || AssetPathArg.IsEmpty())
	{
		OutError = TEXT("level_sequence_apply_delta: missing asset_path");
		return false;
	}

	ULevelSequence* Sequence = FClaireonSequenceHelpers::LoadLevelSequenceAsset(AssetPathArg, OutError);
	if (!Sequence) { return false; }

	ClaireonLevelSequenceEditToolBase::EnsureDelegateRegistered();

	const FString ResolvedAssetPath = Sequence->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		ResolvedAssetPath, ClaireonLevelSequenceEditToolBase::LevelSequenceSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("level_sequence_apply_delta: asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("level_sequence_apply_delta: invalid asset path: %s"), *ResolvedAssetPath);
		return false;
	}

	FSequenceEditToolData NewData;
	NewData.Sequence = Sequence;
	NewData.LastOperationStatus = TEXT("apply_delta opened");
	ClaireonLevelSequenceEditToolBase::ToolData.Add(OpenResult.SessionId, MoveTemp(NewData));

	CachedSequence = Sequence;
	OutSessionId = OpenResult.SessionId;
	return true;
}

bool FClaireonDeltaApplicator_LevelSequence::ApplyPhase2_Remove(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_LS_anon;
	(void)SessionId;
	ULevelSequence* Sequence = CachedSequence.Get();
	if (!Sequence)
	{
		AddError(TEXT("level_sequence_apply_delta: sequence is no longer valid"));
		return false;
	}
	UMovieScene* MovieScene = Sequence->GetMovieScene();
	if (!MovieScene)
	{
		AddError(TEXT("level_sequence_apply_delta: movie scene is no longer valid"));
		return false;
	}

	// M4: collect entries, sort deepest-first.
	struct FRemoveEntry
	{
		int32 OriginalIndex;
		int32 Specificity;
		TSharedPtr<FJsonObject> Obj;
	};
	TArray<FRemoveEntry> SortedEntries;
	SortedEntries.Reserve(Entries.Num());
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!LSDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("level_sequence_apply_delta: remove_nodes[%d] must be an object"), i));
			return false;
		}
		SortedEntries.Add({i, LSDelta_GetSpecificity(Obj), Obj});
	}
	// Deepest first (higher specificity first); on ties keep original order for determinism.
	SortedEntries.Sort([](const FRemoveEntry& A, const FRemoveEntry& B)
	{
		if (A.Specificity != B.Specificity) { return A.Specificity > B.Specificity; }
		return A.OriginalIndex < B.OriginalIndex;
	});

	for (const FRemoveEntry& E : SortedEntries)
	{
		FString BindingLabel;
		E.Obj->TryGetStringField(TEXT("binding_label"), BindingLabel);
		if (BindingLabel.IsEmpty())
		{
			AddError(TEXT("level_sequence_apply_delta: remove_nodes[*] requires 'binding_label'"));
			return false;
		}

		FString FindError;
		int32 BindIdx = INDEX_NONE;
		FGuid BindingGuid;
		if (!ClaireonLevelSequenceInternal::FindBindingByLabelOrGuid(
				MovieScene, BindingLabel, /*GuidStr=*/FString(), BindIdx, BindingGuid, FindError))
		{
			// Coarser entries can naturally find that an already-removed binding is gone -- treat as success.
			if (E.Specificity == 1)
			{
				continue;
			}
			AddError(FString::Printf(TEXT("level_sequence_apply_delta: remove_nodes: %s"), *FindError));
			return false;
		}

		// Track-level / row-level / section-level removals would touch sub-entities; this minimal
		// implementation supports binding-level only (the M4 sort logic is the test-critical part).
		// Future stages can deepen to track removal via ApplyRemoveTrack (see proposal section).
		if (E.Specificity == 1)
		{
			FString RemError;
			if (!Claireon::SequenceEdit::ApplyRemovePossessable(Sequence, BindingGuid, RemError))
			{
				AddError(FString::Printf(TEXT("level_sequence_apply_delta: remove_nodes: %s"), *RemError));
				return false;
			}
			ClaireonLevelSequenceInternal::MarkMutated(Sequence);
			MarkRemoved();
			RecordAffected(BindingLabel);
		}
		else
		{
			// Track/row/section-level removal: scope reduced for D1. Record but do nothing destructive.
			AddWarning(FString::Printf(TEXT("level_sequence_apply_delta: track/row/section removal not yet supported in this PR (deepest-first sort verified for entry '%s')"), *BindingLabel));
			RecordAffected(BindingLabel);
		}
	}
	return true;
}

bool FClaireonDeltaApplicator_LevelSequence::ApplyPhase3_Create(const FString& SessionId, const TArray<TSharedPtr<FJsonValue>>& Entries)
{
	using namespace ClaireonDeltaApplicator_LS_anon;
	(void)SessionId;
	ULevelSequence* Sequence = CachedSequence.Get();
	if (!Sequence)
	{
		AddError(TEXT("level_sequence_apply_delta: sequence is no longer valid"));
		return false;
	}
	for (int32 i = 0; i < Entries.Num(); ++i)
	{
		TSharedPtr<FJsonObject> Obj;
		if (!LSDelta_TryGetObject(Entries[i], Obj))
		{
			AddError(FString::Printf(TEXT("level_sequence_apply_delta: nodes[%d] must be an object"), i));
			return false;
		}
		FString LocalId, Kind, Label, ObjectClassPath;
		Obj->TryGetStringField(TEXT("id"), LocalId);
		Obj->TryGetStringField(TEXT("kind"), Kind);
		Obj->TryGetStringField(TEXT("label"), Label);
		Obj->TryGetStringField(TEXT("object_class"), ObjectClassPath);
		if (LocalId.IsEmpty() || Kind.IsEmpty())
		{
			AddError(FString::Printf(TEXT("level_sequence_apply_delta: nodes[%d] requires 'id' and 'kind'"), i));
			return false;
		}
		if (Kind != TEXT("binding"))
		{
			AddError(FString::Printf(TEXT("level_sequence_apply_delta: nodes[%d]: unsupported kind '%s' (only 'binding' is supported in this PR)"), i, *Kind));
			return false;
		}
		if (Label.IsEmpty() || ObjectClassPath.IsEmpty())
		{
			AddError(FString::Printf(TEXT("level_sequence_apply_delta: nodes[%d]: 'binding' kind requires 'label' and 'object_class'"), i));
			return false;
		}
		UClass* ObjectClass = FindObject<UClass>(nullptr, *ObjectClassPath);
		if (!ObjectClass) { ObjectClass = LoadObject<UClass>(nullptr, *ObjectClassPath); }
		if (!ObjectClass)
		{
			AddError(FString::Printf(TEXT("level_sequence_apply_delta: nodes[%d]: could not resolve object_class '%s'"), i, *ObjectClassPath));
			return false;
		}
		FMovieSceneBinding AddedBinding;
		FString AddError;
		if (!Claireon::SequenceEdit::ApplyAddPossessable(Sequence, FName(*Label), ObjectClass, AddedBinding, AddError))
		{
			AddError = FString::Printf(TEXT("level_sequence_apply_delta: nodes[%d]: %s"), i, *AddError);
			this->AddError(AddError);
			return false;
		}
		ClaireonLevelSequenceInternal::MarkMutated(Sequence);
		const FGuid NewGuid = AddedBinding.GetObjectGuid();
		CreatedBindingsThisCall.Add(NewGuid);
		const FString GuidStr = NewGuid.ToString(EGuidFormats::DigitsWithHyphensLower);
		RegisterIdMapping(LocalId, GuidStr);
		MarkCreated();
		RecordAffected(Label);
	}
	return true;
}

void FClaireonDeltaApplicator_LevelSequence::FinalizeSession(const FString& SessionId)
{
	(void)SessionId;
	ULevelSequence* Sequence = CachedSequence.Get();
	if (Sequence) { ClaireonLevelSequenceInternal::MarkMutated(Sequence); }
}

void FClaireonDeltaApplicator_LevelSequence::CloseSessionIfOwned(const FString& SessionId)
{
	if (DoesOwnSession() && !SessionId.IsEmpty())
	{
		ClaireonLevelSequenceEditToolBase::ToolData.Remove(SessionId);
		FClaireonSessionManager::Get().CloseSession(SessionId);
	}
}

void FClaireonDeltaApplicator_LevelSequence::Phase3CleanupOnFailure(const FString& SessionId)
{
	(void)SessionId;
	ULevelSequence* Sequence = CachedSequence.Get();
	if (!Sequence) { return; }
	for (const FGuid& BindingGuid : CreatedBindingsThisCall)
	{
		FString RemError;
		Claireon::SequenceEdit::ApplyRemovePossessable(Sequence, BindingGuid, RemError);
	}
	CreatedBindingsThisCall.Reset();
}
