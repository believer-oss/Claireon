// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Misc/FrameNumber.h"
#include "KeyParams.h"
#include "Tools/ClaireonSequenceHelpers.h"

class AActor;
class ULevelSequence;
class UClass;
class UMovieSceneTrack;
class UMovieSceneSection;
class UBlueprint;
class UFunction;
struct FMovieSceneBinding;

/**
 * F2 apply-handler surface -- module-private header so F6's spec applicator
 * (FClaireonSpecApplicator_LevelSequence) can reuse the same per-operation logic
 * without round-tripping through the MCP dispatcher.
 *
 * Definitions live in ClaireonTool_SequenceEdit.cpp. No static qualifier so the
 * linker emits a single definition that both translation units can call.
 *
 * See FEATURE_6_CLAIREON_SPEC_APPLICATOR.md "Handler extraction requirement".
 */

bool ApplyAddPossessable(ULevelSequence* Sequence, FName Label, UClass* ObjectClass,
	FMovieSceneBinding& OutBinding, FString& OutError);

bool ApplyRemovePossessable(ULevelSequence* Sequence, const FGuid& Guid, FString& OutError);

bool ApplyAddTrack(ULevelSequence* Sequence, const FGuid& BindingGuid, UClass* TrackClass,
	UMovieSceneTrack*& OutTrack, FString& OutError);

bool ApplyRemoveTrack(ULevelSequence* Sequence, const FGuid& BindingGuid, int32 TrackIndex,
	FString& OutError);

bool ApplyAddSection(UMovieSceneTrack* Track, FFrameNumber Start, FFrameNumber End, int32 RowIndex,
	UMovieSceneSection*& OutSection, FString& OutError);

bool ApplyRemoveSection(UMovieSceneTrack* Track, int32 SectionIndex, FString& OutError);

bool ApplyAddKeyframe(UMovieSceneSection* Section, FFrameNumber Frame, const FString& ValueJson,
	FString& OutError);

bool ApplyRemoveKeyframe(UMovieSceneSection* Section, FFrameNumber Frame, FString& OutError);

/**
 * Applies an interpolation (tangent) mode to the key at the given frame on all
 * interpolation-bearing channels of the given section. Supports float / double /
 * integer channels explicitly (integer has no per-key interp mode in the engine,
 * so the call succeeds as a no-op once the key is located). For any other
 * channel type (bool, byte, string, event, etc.) where a key exists at the frame
 * but the channel does not carry per-key interpolation, returns an explicit
 * error via OutError rather than silently ignoring the request.
 *
 * Returns true on success, false with OutError populated on failure.
 */
bool ApplySetKeyInterpMode(UMovieSceneSection* Section, FFrameNumber Frame,
	EMovieSceneKeyInterpolation InterpMode, FString& OutError);

bool ApplyCreateEventEndpoint(ULevelSequence* Sequence, FName EndpointName,
	ESequenceEventEndpointSignature Signature, UBlueprint*& OutDirectorBP, UFunction*& OutFunction,
	FString& OutError);

/**
 * Re-attach a world AActor to an existing possessable binding GUID inside the
 * given ULevelSequence, preserving the GUID so all sections / tracks already
 * referencing it continue to play unchanged.
 *
 * Steps performed:
 *   1. Validate Sequence and its UMovieScene are non-null.
 *   2. Look up FMovieScenePossessable* via UMovieScene::FindPossessable.
 *      Return false if the GUID is not a possessable (could be a spawnable,
 *      could be missing entirely).
 *   3. Reject if FMovieScenePossessable::GetParent().IsValid() (i.e.
 *      ULevelSequence::CanRebindPossessable returns false for child / component
 *      possessables).
 *   4. Reject if (bClear == false && Actor == nullptr).
 *   5. Defensive class compatibility (#if WITH_EDITORONLY_DATA only): if
 *      Possessable->GetPossessedObjectClass() is non-null and Actor is non-null
 *      and !Actor->GetClass()->IsChildOf(...), reject.
 *   6. Sequence->UnbindPossessableObjects(BindingGuid)  -- drops ALL existing
 *      binding references for the GUID.
 *   7. If !bClear: Sequence->BindPossessableObject(BindingGuid, *Actor,
 *      EditorWorld) where EditorWorld is GEditor->GetEditorWorldContext().World().
 *
 * Returns true on success, false with OutError populated on failure.
 *
 * Actor may be nullptr only when bClear is true. The handler does NOT take a
 * World*; if GEditor is null, returns false.
 */
bool ApplyRebindActor(
    ULevelSequence* Sequence,
    const FGuid&    BindingGuid,
    AActor*         Actor,        // may be null when bClear is true
    bool            bClear,
    FString&        OutError);
