// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class ULevelSequence;
class UMovieScene;
class UMovieSceneSection;
class UMovieSceneTrack;
class UBlueprint;
class UFunction;
struct FMovieSceneBinding;
struct FKeyHandle;
struct FMovieSceneChannelProxy;

/**
 * Event endpoint signature used when creating Director Blueprint endpoints
 * via sequence_edit.create_event_endpoint (F5 -- stage 013).
 *
 * NOTE: This is a plain C++ enum (not UENUM) because this header has no
 * .generated.h; the enum is only referenced from C++ API signatures.
 */
enum class ESequenceEventEndpointSignature : uint8
{
	NoParams,
	BoundObject
};

/**
 * Helpers for Level Sequence MCP tools (F1 + F5).
 *
 * - F1 surface: asset loading, structure formatting, class resolution, frame/time coercion.
 * - F5 surface: Director Blueprint ensure + event endpoint creation (stage 013 impls).
 *
 * All methods are stubs in stage 001 and return safe defaults with TODO error strings.
 */
class CLAIREON_API FClaireonSequenceHelpers
{
public:
	// F1 API (impls in stage 003)
	static ULevelSequence* LoadLevelSequenceAsset(const FString& Path, FString& OutError);
	static FString FormatSequenceStructure(const ULevelSequence* Sequence, bool bIncludeKeyframes, bool bIncludeSections);
	static UClass* ResolveTrackClass(const FString& TypeName);
	static FString FormatBinding(const FMovieSceneBinding& Binding, const UMovieScene* MovieScene);
	static FString FormatTrack(const UMovieSceneTrack* Track, bool bIncludeSections);
	static FString FormatSection(const UMovieSceneSection* Section);
	static FString FormatKeyframe(const FKeyHandle& Handle, const FMovieSceneChannelProxy& Channels);
	static bool CoerceKeyframeValue(UMovieSceneTrack* Track, const FString& JsonPayload, FFrameNumber Frame, FString& OutError);
	static FFrameNumber SecondsToFrame(const ULevelSequence* Sequence, double Seconds);
	static double FrameToSeconds(const ULevelSequence* Sequence, FFrameNumber Frame);

	// F5 API (impls in stage 013)
	static UBlueprint* EnsureDirectorBlueprint(ULevelSequence* Sequence, FString& OutError);
	static UFunction* CreateEventEndpointNode(UBlueprint* DirectorBlueprint, FName EndpointName, ESequenceEventEndpointSignature Signature, FString& OutError);
};
