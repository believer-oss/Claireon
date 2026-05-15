// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSpecApplicator_LevelSequence.h"
#include "Tools/ClaireonSequenceHelpers.h"
#include "ClaireonSequenceEditHandlers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "FileHelpers.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/Blueprint.h"

#include "LevelSequence.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneMarkedFrame.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "Channels/MovieSceneChannelProxy.h"
#include "Channels/MovieSceneChannel.h"

// ----------------------------------------------------------------------------
// Helpers -- local to this TU
// ----------------------------------------------------------------------------

namespace
{
	// Canonicalize a JSON value to a stable string for value-equality comparisons
	// of keyframe payloads. Mirrors the serialization F2 uses on the input side.
	FString CanonicalizeValue(const TSharedPtr<FJsonValue>& Val)
	{
		if (!Val.IsValid())
		{
			return FString();
		}
		if (Val->Type == EJson::Object)
		{
			TSharedPtr<FJsonObject> Obj = Val->AsObject();
			if (!Obj.IsValid())
			{
				return FString();
			}
			FString Out;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
			return Out;
		}
		double NumVal = 0.0;
		if (Val->TryGetNumber(NumVal))
		{
			return FString::Printf(TEXT("%.9g"), NumVal);
		}
		bool BoolVal = false;
		if (Val->TryGetBool(BoolVal))
		{
			return BoolVal ? TEXT("true") : TEXT("false");
		}
		FString StrVal;
		if (Val->TryGetString(StrVal))
		{
			return StrVal;
		}
		return FString();
	}

	// Strip a /Game-style path to the package name and asset name.
	void SplitPackageAndAsset(const FString& Path, FString& OutPackage, FString& OutAsset)
	{
		int32 Dot = INDEX_NONE;
		if (Path.FindChar(TEXT('.'), Dot))
		{
			OutPackage = Path.Left(Dot);
			OutAsset = Path.Mid(Dot + 1);
		}
		else
		{
			OutPackage = Path;
			int32 Slash = INDEX_NONE;
			if (Path.FindLastChar(TEXT('/'), Slash))
			{
				OutAsset = Path.Mid(Slash + 1);
			}
			else
			{
				OutAsset = Path;
			}
		}
	}

	ULevelSequence* CreateLevelSequenceAtPath(const FString& PackagePath, FString& OutError)
	{
		FString PkgName, AssetName;
		SplitPackageAndAsset(PackagePath, PkgName, AssetName);
		if (PkgName.IsEmpty() || AssetName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("invalid package path: %s"), *PackagePath);
			return nullptr;
		}
		UPackage* Package = CreatePackage(*PkgName);
		if (!Package)
		{
			OutError = FString::Printf(TEXT("CreatePackage failed: %s"), *PkgName);
			return nullptr;
		}
		Package->FullyLoad();
		ULevelSequence* NewSeq = NewObject<ULevelSequence>(
			Package, FName(*AssetName),
			RF_Public | RF_Standalone | RF_Transactional);
		if (!NewSeq)
		{
			OutError = TEXT("NewObject<ULevelSequence> failed");
			return nullptr;
		}
		NewSeq->Initialize();
		NewSeq->MarkPackageDirty();
		return NewSeq;
	}

	// Index-identity of a section within a track, matching the spec's identity rule
	// (row_index, start_frame). Returns INDEX_NONE if no matching section exists.
	int32 FindSectionIndexByIdentity(const UMovieSceneTrack* Track, int32 RowIndex, int32 StartFrame)
	{
		if (!Track)
		{
			return INDEX_NONE;
		}
		const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
		for (int32 i = 0; i < Sections.Num(); ++i)
		{
			if (!Sections[i])
			{
				continue;
			}
			const TRange<FFrameNumber> Range = Sections[i]->GetRange();
			if (!Range.GetLowerBound().IsClosed())
			{
				continue;
			}
			const int32 Start = Range.GetLowerBoundValue().Value;
			if (Sections[i]->GetRowIndex() == RowIndex && Start == StartFrame)
			{
				return i;
			}
		}
		return INDEX_NONE;
	}
}

// ----------------------------------------------------------------------------
// ValidateToolSpec
// ----------------------------------------------------------------------------

bool FClaireonSpecApplicator_LevelSequence::ValidateToolSpec(const TSharedPtr<FJsonObject>& Spec, TArray<FString>& OutErrors)
{
	if (!Spec.IsValid())
	{
		OutErrors.Add(TEXT("spec is null"));
		return false;
	}

	bool bHasContent = false;
	const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
	if (Spec->TryGetArrayField(TEXT("bindings"), Bindings) && Bindings)
	{
		bHasContent = true;
		for (int32 i = 0; i < Bindings->Num(); ++i)
		{
			const TSharedPtr<FJsonValue>& Val = (*Bindings)[i];
			if (!Val.IsValid() || Val->Type != EJson::Object)
			{
				OutErrors.Add(FString::Printf(TEXT("bindings[%d]: not an object"), i));
				continue;
			}
			const TSharedPtr<FJsonObject>& Obj = Val->AsObject();
			FString Label;
			if (!Obj->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
			{
				OutErrors.Add(FString::Printf(TEXT("bindings[%d]: missing/empty 'label'"), i));
			}
			FString Kind;
			Obj->TryGetStringField(TEXT("kind"), Kind);
			if (!Kind.IsEmpty() && Kind != TEXT("possessable") && Kind != TEXT("spawnable") && Kind != TEXT("root"))
			{
				OutErrors.Add(FString::Printf(
					TEXT("bindings[%d]: unknown kind '%s' (valid: possessable|spawnable|root)"), i, *Kind));
			}
		}
	}

	if (Spec->HasField(TEXT("playback_range")))
	{
		bHasContent = true;
	}
	if (Spec->HasField(TEXT("marked_frames")))
	{
		bHasContent = true;
	}

	if (!bHasContent)
	{
		OutErrors.Add(TEXT("LevelSequence spec must contain at least one of: 'bindings', 'playback_range', 'marked_frames'"));
		return false;
	}
	return OutErrors.Num() == 0;
}

// ----------------------------------------------------------------------------
// OpenOrCreateAsset
// ----------------------------------------------------------------------------

bool FClaireonSpecApplicator_LevelSequence::OpenOrCreateAsset(const FString& AssetPath, FString& OutSessionId, FString& OutError)
{
	ClaireonPathResolver::FResolveResult Resolve = ClaireonPathResolver::Resolve(AssetPath);
	if (!Resolve.bSuccess)
	{
		OutError = Resolve.Error;
		return false;
	}
	const FString Resolved = Resolve.ResolvedPath.Path;

	FString LoadError;
	ULevelSequence* LS = FClaireonSequenceHelpers::LoadLevelSequenceAsset(Resolved, LoadError);
	if (!LS)
	{
		// Fall back to creation: Claireon's apply_spec is create-if-missing by default.
		FString CreateError;
		FString PackageName = Resolved;
		int32 Dot = INDEX_NONE;
		if (PackageName.FindChar(TEXT('.'), Dot))
		{
			PackageName = PackageName.Left(Dot);
		}
		LS = CreateLevelSequenceAtPath(PackageName, CreateError);
		if (!LS)
		{
			OutError = FString::Printf(TEXT("load failed (%s) and create failed (%s)"),
				*LoadError, *CreateError);
			return false;
		}
	}

	const FString FullPath = LS->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		FullPath, TEXT("claireon.sequence_edit"));
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		OutError = FString::Printf(TEXT("Asset is locked by %s session %s"),
			*Blocker.ToolName, *Blocker.SessionId);
		return false;
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		OutError = FString::Printf(TEXT("Invalid asset path: %s"), *FullPath);
		return false;
	}

	Sequence = LS;
	ActiveSessionId = OpenResult.SessionId;
	bOwnsSession = true;
	OutSessionId = ActiveSessionId;
	return true;
}

// ----------------------------------------------------------------------------
// ApplyPass1_CreateEntities -- the full diff+apply happens here.
// Pass 2 is a no-op for level sequences (creation + wiring are the same step).
// ----------------------------------------------------------------------------

bool FClaireonSpecApplicator_LevelSequence::ApplyPass1_CreateEntities(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	ULevelSequence* LS = Sequence.Get();
	if (!LS)
	{
		AddError(TEXT("Level Sequence is no longer valid"));
		return false;
	}
	UMovieScene* MovieScene = LS->GetMovieScene();
	if (!MovieScene)
	{
		AddError(TEXT("Level Sequence has no MovieScene"));
		return false;
	}

	// === 1. Build a label -> binding-index map of the current state ===
	TMap<FString, int32> CurrentBindingIndexByLabel;
	{
		const TArray<FMovieSceneBinding>& Bindings = MovieScene->GetBindings();
		for (int32 i = 0; i < Bindings.Num(); ++i)
		{
			CurrentBindingIndexByLabel.Add(Bindings[i].GetName(), i);
		}
	}

	// Collect spec labels.
	TSet<FString> SpecLabels;
	const TArray<TSharedPtr<FJsonValue>>* SpecBindings = nullptr;
	Spec->TryGetArrayField(TEXT("bindings"), SpecBindings);
	if (SpecBindings)
	{
		for (const TSharedPtr<FJsonValue>& Val : *SpecBindings)
		{
			if (!Val.IsValid() || Val->Type != EJson::Object) continue;
			FString Label;
			if (Val->AsObject()->TryGetStringField(TEXT("label"), Label))
			{
				SpecLabels.Add(Label);
			}
		}
	}

	// === 2. Remove bindings not in the spec (removal-first invariant) ===
	if (SpecBindings)
	{
		TArray<FString> CurrentLabels;
		CurrentBindingIndexByLabel.GenerateKeyArray(CurrentLabels);
		for (const FString& Label : CurrentLabels)
		{
			if (SpecLabels.Contains(Label))
			{
				continue;
			}
			// Find the GUID for this label.
			FGuid Guid;
			for (const FMovieSceneBinding& B : MovieScene->GetBindings())
			{
				if (B.GetName() == Label)
				{
					Guid = B.GetObjectGuid();
					break;
				}
			}
			FString Err;
			if (ApplyRemovePossessable(LS, Guid, Err))
			{
				RecordEntrySuccess(FString::Printf(TEXT("binding:%s:remove"), *Label), Guid.ToString());
			}
			else
			{
				AddWarning(FString::Printf(TEXT("remove binding '%s' failed: %s"), *Label, *Err));
			}
		}
	}

	// === 3. Walk spec bindings and converge ===
	if (SpecBindings)
	{
		for (int32 BIdx = 0; BIdx < SpecBindings->Num(); ++BIdx)
		{
			const TSharedPtr<FJsonValue>& BVal = (*SpecBindings)[BIdx];
			if (!BVal.IsValid() || BVal->Type != EJson::Object) continue;
			const TSharedPtr<FJsonObject>& BObj = BVal->AsObject();

			FString Label;
			BObj->TryGetStringField(TEXT("label"), Label);
			if (Label.IsEmpty()) continue;

			FString Kind = TEXT("possessable");
			BObj->TryGetStringField(TEXT("kind"), Kind);

			FGuid BindingGuid;
			int32* ExistingIdxPtr = CurrentBindingIndexByLabel.Find(Label);
			if (ExistingIdxPtr && *ExistingIdxPtr >= 0 && *ExistingIdxPtr < MovieScene->GetBindings().Num())
			{
				BindingGuid = MovieScene->GetBindings()[*ExistingIdxPtr].GetObjectGuid();
			}
			else if (Kind != TEXT("root"))
			{
				// Add possessable (spawnables are advanced; treat as possessable for v1).
				FString ObjectClassPath;
				BObj->TryGetStringField(TEXT("object_class"), ObjectClassPath);
				UClass* ObjectClass = nullptr;
				if (!ObjectClassPath.IsEmpty())
				{
					ObjectClass = FindObject<UClass>(nullptr, *ObjectClassPath);
					if (!ObjectClass)
					{
						ObjectClass = LoadObject<UClass>(nullptr, *ObjectClassPath);
					}
				}
				if (!ObjectClass)
				{
					RecordEntryFailure(FString::Printf(TEXT("binding:%s"), *Label),
						FString::Printf(TEXT("cannot resolve object_class '%s'"), *ObjectClassPath));
					continue;
				}
				FMovieSceneBinding NewBinding;
				FString Err;
				if (!ApplyAddPossessable(LS, FName(*Label), ObjectClass, NewBinding, Err))
				{
					RecordEntryFailure(FString::Printf(TEXT("binding:%s"), *Label), Err);
					continue;
				}
				BindingGuid = NewBinding.GetObjectGuid();
				RecordEntrySuccess(FString::Printf(TEXT("binding:%s:add"), *Label), BindingGuid.ToString());
			}
			// else: "root" binding -- no-op, tracks live at scene root.

			// --- Track convergence within this binding ---
			const TArray<TSharedPtr<FJsonValue>>* SpecTracks = nullptr;
			BObj->TryGetArrayField(TEXT("tracks"), SpecTracks);
			if (!SpecTracks) continue;

			// Gather current track types for identity matching.
			auto FetchCurrentTracks = [&](TArray<UMovieSceneTrack*>& Out)
			{
				Out.Reset();
				if (BindingGuid.IsValid())
				{
					for (const FMovieSceneBinding& B : MovieScene->GetBindings())
					{
						if (B.GetObjectGuid() == BindingGuid)
						{
							Out.Append(B.GetTracks());
							return;
						}
					}
				}
				else
				{
					Out.Append(MovieScene->GetTracks());
				}
			};

			// Collect spec track types so we can remove orphans.
			TSet<FString> SpecTrackTypes;
			for (const TSharedPtr<FJsonValue>& TVal : *SpecTracks)
			{
				if (!TVal.IsValid() || TVal->Type != EJson::Object) continue;
				FString Type;
				TVal->AsObject()->TryGetStringField(TEXT("type"), Type);
				if (!Type.IsEmpty())
				{
					SpecTrackTypes.Add(Type.ToLower());
				}
			}

			// Remove tracks whose type isn't in spec.
			{
				TArray<UMovieSceneTrack*> CurTracks;
				FetchCurrentTracks(CurTracks);
				for (int32 TrackIdx = CurTracks.Num() - 1; TrackIdx >= 0; --TrackIdx)
				{
					UMovieSceneTrack* T = CurTracks[TrackIdx];
					if (!T) continue;
					// Recover a spec type name via our helper. If none, leave it alone.
					FString FoundType;
					for (const auto& Pair : { TEXT("transform"), TEXT("visibility"), TEXT("event"),
											   TEXT("audio"), TEXT("camera_cut"), TEXT("float") })
					{
						UClass* C = FClaireonSequenceHelpers::ResolveTrackClass(Pair);
						if (C && T->GetClass() == C)
						{
							FoundType = FString(Pair).ToLower();
							break;
						}
					}
					if (FoundType.IsEmpty() || SpecTrackTypes.Contains(FoundType))
					{
						continue;
					}
					FString Err;
					if (!ApplyRemoveTrack(LS, BindingGuid, TrackIdx, Err))
					{
						AddWarning(FString::Printf(TEXT("remove track [%d] on '%s' failed: %s"),
							TrackIdx, *Label, *Err));
					}
				}
			}

			// Add missing tracks; reuse if type already present.
			for (const TSharedPtr<FJsonValue>& TVal : *SpecTracks)
			{
				if (!TVal.IsValid() || TVal->Type != EJson::Object) continue;
				const TSharedPtr<FJsonObject>& TObj = TVal->AsObject();
				FString Type;
				TObj->TryGetStringField(TEXT("type"), Type);
				if (Type.IsEmpty()) continue;

				UClass* TrackClass = FClaireonSequenceHelpers::ResolveTrackClass(Type);
				if (!TrackClass)
				{
					RecordEntryFailure(
						FString::Printf(TEXT("track:%s:%s"), *Label, *Type),
						FString::Printf(TEXT("unknown track_type '%s'"), *Type));
					continue;
				}

				// Find-first-by-type (identity rule for F6).
				UMovieSceneTrack* Track = nullptr;
				{
					TArray<UMovieSceneTrack*> CurTracks;
					FetchCurrentTracks(CurTracks);
					for (UMovieSceneTrack* T : CurTracks)
					{
						if (T && T->GetClass() == TrackClass)
						{
							Track = T;
							break;
						}
					}
				}
				if (!Track)
				{
					FString Err;
					if (!ApplyAddTrack(LS, BindingGuid, TrackClass, Track, Err))
					{
						RecordEntryFailure(
							FString::Printf(TEXT("track:%s:%s"), *Label, *Type), Err);
						continue;
					}
					RecordEntrySuccess(
						FString::Printf(TEXT("track:%s:%s:add"), *Label, *Type),
						Track ? Track->GetName() : FString());
				}
				if (!Track) continue;

				// --- Section convergence within this track ---
				const TArray<TSharedPtr<FJsonValue>>* SpecSections = nullptr;
				TObj->TryGetArrayField(TEXT("sections"), SpecSections);
				if (!SpecSections) continue;

				// Build spec identity set for remove.
				TSet<FString> SpecSectionIds;
				for (const TSharedPtr<FJsonValue>& SVal : *SpecSections)
				{
					if (!SVal.IsValid() || SVal->Type != EJson::Object) continue;
					const TSharedPtr<FJsonObject>& SObj = SVal->AsObject();
					int32 Start = 0, Row = 0;
					SObj->TryGetNumberField(TEXT("start_frame"), Start);
					SObj->TryGetNumberField(TEXT("row_index"), Row);
					SpecSectionIds.Add(FString::Printf(TEXT("%d:%d"), Row, Start));
				}

				// Remove sections whose (row,start) isn't in spec.
				{
					const TArray<UMovieSceneSection*>& CurSections = Track->GetAllSections();
					for (int32 i = CurSections.Num() - 1; i >= 0; --i)
					{
						UMovieSceneSection* S = CurSections[i];
						if (!S) continue;
						const TRange<FFrameNumber> R = S->GetRange();
						if (!R.GetLowerBound().IsClosed()) continue;
						const FString Id = FString::Printf(TEXT("%d:%d"),
							S->GetRowIndex(), R.GetLowerBoundValue().Value);
						if (!SpecSectionIds.Contains(Id))
						{
							FString Err;
							if (!ApplyRemoveSection(Track, i, Err))
							{
								AddWarning(FString::Printf(TEXT("remove section failed: %s"), *Err));
							}
						}
					}
				}

				// Add or update each spec section.
				for (const TSharedPtr<FJsonValue>& SVal : *SpecSections)
				{
					if (!SVal.IsValid() || SVal->Type != EJson::Object) continue;
					const TSharedPtr<FJsonObject>& SObj = SVal->AsObject();
					int32 Start = 0, End = 0, Row = 0;
					SObj->TryGetNumberField(TEXT("start_frame"), Start);
					SObj->TryGetNumberField(TEXT("end_frame"), End);
					SObj->TryGetNumberField(TEXT("row_index"), Row);

					int32 SecIdx = FindSectionIndexByIdentity(Track, Row, Start);
					UMovieSceneSection* Section = nullptr;
					if (SecIdx == INDEX_NONE)
					{
						FString Err;
						if (!ApplyAddSection(Track, FFrameNumber(Start), FFrameNumber(End), Row, Section, Err))
						{
							RecordEntryFailure(
								FString::Printf(TEXT("section:%s:%s:%d@%d"), *Label, *Type, Row, Start),
								Err);
							continue;
						}
						RecordEntrySuccess(
							FString::Printf(TEXT("section:%s:%s:%d@%d:add"), *Label, *Type, Row, Start),
							FString::Printf(TEXT("[%d,%d)"), Start, End));
					}
					else
					{
						Section = Track->GetAllSections()[SecIdx];
					}
					if (!Section) continue;

					// --- Keyframe convergence ---
					const TArray<TSharedPtr<FJsonValue>>* SpecKeys = nullptr;
					SObj->TryGetArrayField(TEXT("keyframes"), SpecKeys);

					// Collect spec frames for removal of extras.
					TSet<int32> SpecFrames;
					if (SpecKeys)
					{
						for (const TSharedPtr<FJsonValue>& KVal : *SpecKeys)
						{
							if (!KVal.IsValid() || KVal->Type != EJson::Object) continue;
							int32 Frame = 0;
							KVal->AsObject()->TryGetNumberField(TEXT("frame"), Frame);
							SpecFrames.Add(Frame);
						}
					}

					// Remove keyframes whose frame isn't in spec.
					{
						FMovieSceneChannelProxy& Proxy = Section->GetChannelProxy();
						TSet<int32> CurrentFrames;
						for (const FMovieSceneChannelEntry& Entry : Proxy.GetAllEntries())
						{
							for (FMovieSceneChannel* Channel : Entry.GetChannels())
							{
								if (!Channel) continue;
								TArray<FFrameNumber> Times;
								Channel->GetKeys(TRange<FFrameNumber>::All(), &Times, nullptr);
								for (const FFrameNumber& T : Times)
								{
									CurrentFrames.Add(T.Value);
								}
							}
						}
						for (int32 F : CurrentFrames)
						{
							if (!SpecFrames.Contains(F))
							{
								FString Err;
								if (!ApplyRemoveKeyframe(Section, FFrameNumber(F), Err))
								{
									AddWarning(FString::Printf(TEXT("remove key @%d failed: %s"), F, *Err));
								}
							}
						}
					}

					// Add/update spec keyframes.
					if (SpecKeys)
					{
						for (const TSharedPtr<FJsonValue>& KVal : *SpecKeys)
						{
							if (!KVal.IsValid() || KVal->Type != EJson::Object) continue;
							const TSharedPtr<FJsonObject>& KObj = KVal->AsObject();
							int32 Frame = 0;
							KObj->TryGetNumberField(TEXT("frame"), Frame);

							// Event endpoints use a different key payload -- name references the
							// Director function; it must already exist (F5 creates endpoints).
							FString EndpointName;
							KObj->TryGetStringField(TEXT("endpoint"), EndpointName);
							if (!EndpointName.IsEmpty())
							{
								UBlueprint* DirBP = LS->GetDirectorBlueprint();
								UClass* DirClass = DirBP ? Cast<UClass>(DirBP->GeneratedClass) : nullptr;
								UFunction* Fn = DirClass ? DirClass->FindFunctionByName(FName(*EndpointName)) : nullptr;
								if (!Fn)
								{
									RecordEntryFailure(
										FString::Printf(TEXT("keyframe:%s:%s:%d:%s"), *Label, *Type, Frame, *EndpointName),
										FString::Printf(TEXT("unknown event endpoint '%s' (create via F5 first)"), *EndpointName));
									continue;
								}
								// Serialize the payload into the ApplyAddKeyframe input convention.
								FString Payload = FString::Printf(TEXT("\"%s\""), *EndpointName);
								FString Err;
								if (!ApplyAddKeyframe(Section, FFrameNumber(Frame), Payload, Err))
								{
									RecordEntryFailure(
										FString::Printf(TEXT("keyframe:%s:%s:%d:%s"), *Label, *Type, Frame, *EndpointName), Err);
									continue;
								}
								RecordEntrySuccess(
									FString::Printf(TEXT("keyframe:%s:%s:%d:%s:add"), *Label, *Type, Frame, *EndpointName),
									FString::Printf(TEXT("frame=%d"), Frame));
								continue;
							}

							// Generic value payload.
							TSharedPtr<FJsonValue> Val = KObj->TryGetField(TEXT("value"));
							FString ValueJson = CanonicalizeValue(Val);
							if (ValueJson.IsEmpty())
							{
								RecordEntryFailure(
									FString::Printf(TEXT("keyframe:%s:%s:%d"), *Label, *Type, Frame),
									TEXT("missing 'value' or 'endpoint'"));
								continue;
							}
							FString Err;
							if (!ApplyAddKeyframe(Section, FFrameNumber(Frame), ValueJson, Err))
							{
								RecordEntryFailure(
									FString::Printf(TEXT("keyframe:%s:%s:%d"), *Label, *Type, Frame), Err);
								continue;
							}
							RecordEntrySuccess(
								FString::Printf(TEXT("keyframe:%s:%s:%d:upsert"), *Label, *Type, Frame),
								FString::Printf(TEXT("frame=%d"), Frame));
						}
					}
				}
			}
		}
	}

	// === 4. Playback range ===
	const TSharedPtr<FJsonObject>* RangeObj = nullptr;
	if (Spec->TryGetObjectField(TEXT("playback_range"), RangeObj) && RangeObj && RangeObj->IsValid())
	{
		int32 Start = 0, End = 0;
		(*RangeObj)->TryGetNumberField(TEXT("start_frame"), Start);
		(*RangeObj)->TryGetNumberField(TEXT("end_frame"), End);
		const TRange<FFrameNumber> Cur = MovieScene->GetPlaybackRange();
		const int32 CurStart = Cur.GetLowerBound().IsClosed() ? Cur.GetLowerBoundValue().Value : 0;
		const int32 CurEndExclusive = Cur.GetUpperBound().IsClosed() ? Cur.GetUpperBoundValue().Value : 0;
		if (End > Start && (CurStart != Start || CurEndExclusive != End))
		{
			MovieScene->SetPlaybackRange(FFrameNumber(Start), End - Start);
			RecordEntrySuccess(TEXT("playback_range:set"),
				FString::Printf(TEXT("[%d,%d)"), Start, End));
		}
	}

	// === 5. Marked frames -- authoritative replace when provided ===
	const TArray<TSharedPtr<FJsonValue>>* Marked = nullptr;
	if (Spec->TryGetArrayField(TEXT("marked_frames"), Marked) && Marked)
	{
		// Canonicalize desired state.
		TMap<int32, FString> DesiredMarks;
		for (const TSharedPtr<FJsonValue>& MVal : *Marked)
		{
			if (!MVal.IsValid() || MVal->Type != EJson::Object) continue;
			int32 Frame = 0;
			FString LabelText;
			MVal->AsObject()->TryGetNumberField(TEXT("frame"), Frame);
			MVal->AsObject()->TryGetStringField(TEXT("label"), LabelText);
			DesiredMarks.Add(Frame, LabelText);
		}
		// Compare to current.
		const TArray<FMovieSceneMarkedFrame>& Current = MovieScene->GetMarkedFrames();
		bool bEqual = Current.Num() == DesiredMarks.Num();
		if (bEqual)
		{
			for (const FMovieSceneMarkedFrame& Mark : Current)
			{
				const FString* Want = DesiredMarks.Find(Mark.FrameNumber.Value);
				if (!Want || *Want != Mark.Label)
				{
					bEqual = false;
					break;
				}
			}
		}
		if (!bEqual)
		{
			// Clear then re-add. This is spec-authoritative.
			while (MovieScene->GetMarkedFrames().Num() > 0)
			{
				MovieScene->DeleteMarkedFrame(0);
			}
			for (const TPair<int32, FString>& Pair : DesiredMarks)
			{
				FMovieSceneMarkedFrame M(FFrameNumber(Pair.Key));
				M.Label = Pair.Value;
				MovieScene->AddMarkedFrame(M);
			}
			RecordEntrySuccess(TEXT("marked_frames:set"),
				FString::Printf(TEXT("count=%d"), DesiredMarks.Num()));
		}
	}

	LS->MarkPackageDirty();
	MovieScene->MarkAsChanged();
	return true;
}

bool FClaireonSpecApplicator_LevelSequence::ApplyPass2_WireRelationships(const FString& SessionId, const TSharedPtr<FJsonObject>& Spec)
{
	// Level sequences don't split create/wire into two passes -- section/track
	// wiring is part of Pass 1's diff. Intentional no-op.
	UE_LOG(LogClaireon, Log, TEXT("[apply_spec:LevelSequence] Pass 2: no-op (wiring handled in Pass 1)"));
	return true;
}

bool FClaireonSpecApplicator_LevelSequence::CompileAsset(const FString& SessionId, FString& OutError)
{
	// Level sequences don't require asset compilation. The Director BP (if any)
	// is compiled when endpoints are created via F5; apply_spec does not create
	// new endpoints (requires pre-existing).
	return true;
}

bool FClaireonSpecApplicator_LevelSequence::SaveAsset(const FString& SessionId, FString& OutError)
{
	ULevelSequence* LS = Sequence.Get();
	if (!LS)
	{
		OutError = TEXT("sequence pointer invalidated");
		return false;
	}
	UPackage* Package = LS->GetOutermost();
	if (!Package)
	{
		OutError = TEXT("sequence has no package");
		return false;
	}
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		OutError = TEXT("Save blocked: editor state may be corrupted after a previous crash.");
		return false;
	}
	Package->SetDirtyFlag(true);
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages({ Package }, true);
	if (!bSaved)
	{
		OutError = TEXT("SavePackages returned false");
		return false;
	}
	return true;
}

void FClaireonSpecApplicator_LevelSequence::CloseSession(const FString& SessionId)
{
	if (bOwnsSession && !SessionId.IsEmpty())
	{
		FClaireonSessionManager::Get().CloseSession(SessionId);
	}
	ActiveSessionId.Reset();
	bOwnsSession = false;
	Sequence.Reset();
}

