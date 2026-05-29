// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Stateless tool: reverse-reference finder for any loaded UObject.
 *
 * Complement to uobject_inspect (forward: read properties of X) and
 * asset_references (static / package-level via AssetRegistry). This tool
 * answers the live-memory question: given a UObject, which other loaded
 * UObjects currently hold a UPROPERTY pointing at it, and on which property?
 *
 * Uses FFindReferencersArchive (Runtime/CoreUObject/Serialization) which is
 * the same engine primitive that powers UnrealEd's reference-replacement and
 * Reference Viewer paths. Walks the candidate's FProperty serialization,
 * recording every operator<<(UObject*&) that lands on the target -- so only
 * hard UObject* references (and optionally weak) are detected. Soft refs are
 * out-of-scope: they do not force-load and require a different archive shape;
 * use asset_references for soft / package-level visibility.
 *
 * Component targeting: when the target is an AActor and include_components is
 * true (default), the tool also scans for references to each of the actor's
 * UActorComponent sub-objects. Each hit reports target_kind=='actor' or
 * 'component' and the matching target_path so the caller can see whether the
 * referencer points at the actor itself or at one of its components.
 *
 * Filtering:
 *  - include_archetype_refs (default false): skip CDO / RF_ArchetypeObject
 *    candidates. Most BP instances trivially appear as referencers via their
 *    CDO; suppressing this by default keeps signal-to-noise high.
 *  - include_editor_only (default false): drop entries whose holding
 *    FProperty has CPF_EditorOnly set.
 *  - include_weak (default false): also detect weak-object refs.
 *  - max_results (default 200, clamped to [1, 5000]): cap on emitted entries.
 *    truncated=true on the response indicates the iterator hit the cap; the
 *    scanned_object_count reflects the full iteration regardless.
 *
 * Read-only. Iterates GUObjectArray via TObjectIterator -- cost scales with
 * loaded object count, which in editor can be 100k+. Expect multi-hundred-ms
 * latency on large maps; this is acceptable for diagnostic use.
 */
class CLAIREON_API ClaireonTool_UObjectReferencers : public IClaireonTool
{
public:
	FString GetCategory() const override { return TEXT("uobject"); }
	FString GetOperation() const override { return TEXT("referencers"); }
	FString GetDescription() const override;
	TArray<FString> GetSearchKeywords() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
