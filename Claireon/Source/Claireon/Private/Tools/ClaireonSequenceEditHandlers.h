// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Misc/FrameNumber.h"
#include "KeyParams.h"
#include "Tools/ClaireonSequenceHelpers.h"

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
