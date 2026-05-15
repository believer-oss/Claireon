// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Misc/FrameNumber.h"
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

bool ApplyCreateEventEndpoint(ULevelSequence* Sequence, FName EndpointName,
	ESequenceEventEndpointSignature Signature, UBlueprint*& OutDirectorBP, UFunction*& OutFunction,
	FString& OutError);
