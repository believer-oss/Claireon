// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonLevelSequenceEditInternal.h"
#include "Tools/ClaireonLevelSequenceEditToolBase.h"
#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"

namespace ClaireonLevelSequenceInternal
{
	void MarkMutated(ULevelSequence* Sequence)
	{
		if (!Sequence)
		{
			return;
		}
		if (UMovieScene* MovieScene = Sequence->GetMovieScene())
		{
			MovieScene->MarkAsChanged();
		}
		Sequence->MarkPackageDirty();
	}

	bool FindBindingByLabelOrGuid(UMovieScene* MovieScene, const FString& Label, const FString& GuidStr,
		int32& OutIndex, FGuid& OutGuid, FString& OutError)
	{
		OutIndex = INDEX_NONE;
		OutGuid = FGuid();
		if (!MovieScene)
		{
			OutError = TEXT("MovieScene is null");
			return false;
		}
		const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
		if (!GuidStr.IsEmpty())
		{
			FGuid ParsedGuid;
			if (!FGuid::Parse(GuidStr, ParsedGuid))
			{
				OutError = FString::Printf(TEXT("invalid guid: %s"), *GuidStr);
				return false;
			}
			for (int32 i = 0; i < Bindings.Num(); ++i)
			{
				if (Bindings[i].GetObjectGuid() == ParsedGuid)
				{
					OutIndex = i;
					OutGuid = ParsedGuid;
					return true;
				}
			}
			OutError = FString::Printf(TEXT("binding with guid %s not found"), *GuidStr);
			return false;
		}
		if (!Label.IsEmpty())
		{
			for (int32 i = 0; i < Bindings.Num(); ++i)
			{
				if (Bindings[i].GetName() == Label)
				{
					OutIndex = i;
					OutGuid = Bindings[i].GetObjectGuid();
					return true;
				}
			}
			OutError = FString::Printf(TEXT("binding with label '%s' not found"), *Label);
			return false;
		}
		OutError = TEXT("need either 'label' or 'guid'");
		return false;
	}

	UMovieSceneTrack* ResolveFocusedTrack(UMovieScene* MovieScene, int32 FocusedBinding, int32 FocusedTrack, FString& OutError)
	{
		if (!MovieScene)
		{
			OutError = TEXT("MovieScene is null");
			return nullptr;
		}
		if (FocusedTrack == INDEX_NONE)
		{
			OutError = TEXT("no focused track; call focus_track first");
			return nullptr;
		}
		if (FocusedBinding != INDEX_NONE)
		{
			const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
			if (FocusedBinding < 0 || FocusedBinding >= Bindings.Num())
			{
				OutError = TEXT("focused binding index out of range");
				return nullptr;
			}
			const TArray<UMovieSceneTrack*>& Tracks = Bindings[FocusedBinding].GetTracks();
			if (FocusedTrack < 0 || FocusedTrack >= Tracks.Num())
			{
				OutError = TEXT("focused track index out of range");
				return nullptr;
			}
			return Tracks[FocusedTrack];
		}
		const TArray<UMovieSceneTrack*>& Tracks = MovieScene->GetTracks();
		if (FocusedTrack < 0 || FocusedTrack >= Tracks.Num())
		{
			OutError = TEXT("focused track index out of range (root tracks)");
			return nullptr;
		}
		return Tracks[FocusedTrack];
	}

	UMovieSceneSection* ResolveFocusedSection(FSequenceEditToolData* Data, int32 SectionIndex, FString& OutError)
	{
		if (!Data || !Data->Sequence.IsValid())
		{
			OutError = TEXT("invalid session data");
			return nullptr;
		}
		UMovieScene* MovieScene = Data->Sequence->GetMovieScene();
		UMovieSceneTrack* Track = ResolveFocusedTrack(MovieScene, Data->FocusedBindingIndex, Data->FocusedTrackIndex, OutError);
		if (!Track)
		{
			return nullptr;
		}
		const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
		if (Sections.Num() == 0)
		{
			OutError = TEXT("focused track has no sections");
			return nullptr;
		}
		const int32 Idx = (SectionIndex >= 0) ? SectionIndex : 0;
		if (Idx < 0 || Idx >= Sections.Num())
		{
			OutError = FString::Printf(TEXT("section index %d out of range"), Idx);
			return nullptr;
		}
		return Sections[Idx];
	}

	// Create a new Level Sequence at the given /Game/... package path. Mirrors
	// ULevelSequenceFactoryNew::FactoryCreateNew without depending on the
	// LevelSequenceEditor plugin's private factory header.
	ULevelSequence* CreateLevelSequenceAtPath(const FString& PackagePath, FString& OutError)
	{
		OutError.Reset();
		FString PkgName, AssetName;
		if (!FPackageName::IsValidLongPackageName(PackagePath))
		{
			FString Base, Suffix;
			if (PackagePath.Split(TEXT("."), &Base, &Suffix))
			{
				PkgName = Base;
				AssetName = Suffix;
			}
			else
			{
				OutError = FString::Printf(TEXT("invalid package path: %s"), *PackagePath);
				return nullptr;
			}
		}
		else
		{
			PkgName = PackagePath;
		}
		if (AssetName.IsEmpty())
		{
			AssetName = FPackageName::GetLongPackageAssetName(PkgName);
		}
		UPackage* Package = CreatePackage(*PkgName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("CreatePackage failed for: %s"), *PkgName);
			return nullptr;
		}
		Package->FullyLoad();
		ULevelSequence* NewSeq = NewObject<ULevelSequence>(Package, FName(*AssetName),
			RF_Public | RF_Standalone | RF_Transactional);
		if (!NewSeq)
		{
			OutError = TEXT("NewObject<ULevelSequence> failed");
			return nullptr;
		}
		NewSeq->Initialize();
		FAssetRegistryModule::AssetCreated(NewSeq);
		NewSeq->MarkPackageDirty();
		return NewSeq;
	}
}
