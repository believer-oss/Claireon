// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Dom/JsonObject.h"
#include "Templates/SharedPointer.h"
#include "UObject/WeakObjectPtr.h"

class UObject;

/**
 * Two-pass applicator for audio `apply_spec` manifests.
 *
 * Pass 1 materializes every entry (creates missing `define` entries, loads link-only and existing
 * `define` entries). Pass 2 walks `define` bodies, writes fields through ClaireonPropertyUtils, and
 * resolves `*_ref` cross-references against the `IdToAsset` table built in Pass 1.
 *
 * Any failure cancels the outer FScopedTransaction and ObjectTools::DeleteSingleObject-rollbacks
 * every asset in `AssetsCreatedThisCall`. Link-only and pre-existing `define` entries are not
 * deleted on failure.
 */
class CLAIREON_API FClaireonSpecApplicator_Audio
{
public:
	/** Applies the parsed spec. Returns true on success. On failure, OutError is populated and
	 *  any assets created during Pass 1 are deleted (rollback-on-failure per APPLY_SPEC.md). */
	bool Apply(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError);

	/** Per-cohort applicators for the decomposed apply_spec tools. Each applies a single-asset
	 *  spec where Spec->kind matches the cohort. Per-call rollback state is held in IdToAsset /
	 *  AssetsCreatedThisCall (instance fields).
	 *  See AUDIO_DECOMPOSE_APPLY_SPEC.md "Applicator API change" for the canonical surface. */
	bool ApplyAttenuationSpec(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError);
	bool ApplyConcurrencySpec(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError);
	bool ApplySoundClassSpec(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError);
	bool ApplySoundMixSpec(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError);
	bool ApplySoundCueSpec(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError);
	bool ApplyMetaSoundSpec(const TSharedPtr<FJsonObject>& Spec, FString& OutSummary, FString& OutError);

private:
	// Populated in Pass 1. Stable across Pass 1 -> Pass 2.
	TMap<FString, TWeakObjectPtr<UObject>> IdToAsset;

	// Populated in Pass 1. Used for rollback.
	TArray<TWeakObjectPtr<UObject>> AssetsCreatedThisCall;
};
