// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class AActor;
class UWorld;
class USoundBase;
class FJsonObject;
// TSharedPtr is provided by CoreMinimal.h; forward-declaring it here as
// `template <typename T> class TSharedPtr;` is incorrect -- UE's actual
// declaration is `template <typename ObjectType, ESPMode InMode>` (two
// template params with a default), so the single-param forward decl masks
// the real template and causes "too few template arguments" errors on
// MSVC strict mode.

/**
 * Shared helpers used by the decomposed Claireon audio tools
 * (place_ambient_sound / place_audio_volume / attach_audio_component / set_audio_property).
 *
 * The dispatcher in ClaireonTool_AudioApply has been split into per-op tools per the
 * "one Claireon tool does one verb" invariant; these helpers preserve the common
 * JSON parsing and actor-lookup conventions across the new tools.
 */
namespace ClaireonAudioApplyHelpers
{
	/** Parse a transform object {{location:{x,y,z}}, {rotation:{pitch,yaw,roll}}, {scale:{x,y,z}}} */
	bool ParseTransformField(const TSharedPtr<FJsonObject>& Args, FTransform& OutXform, FString& OutError);

	/** Resolve a USoundBase asset path; returns nullptr and populates OutError on failure. */
	USoundBase* LoadSoundBase(const FString& AssetPath, FString& OutError);

	/** Linear-search the editor world for an actor whose GetActorLabel() matches Label. */
	AActor* FindActorByLabel(UWorld* World, const FString& Label);

	/**
	 * Reflection-write a properties blob onto a target UObject. Each value is stringified for
	 * ClaireonPropertyResolver (matches PlaceActor behavior). Warnings (not failures) are
	 * collected for unsettable fields.
	 */
	void WriteReflectedProperties(class UObject* Target, const TSharedPtr<FJsonObject>& Props, TArray<FString>& OutWarnings);
}
