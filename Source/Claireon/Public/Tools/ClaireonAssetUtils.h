// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "Dom/JsonObject.h"

/**
 * Generic asset discovery, loading, and saving utilities.
 * Handles dual-mode loading: Blueprint CDO vs native UObject assets.
 * No game-specific dependencies — works with any Unreal asset type.
 */
namespace ClaireonAssetUtils
{
	/**
	 * Load an asset for property editing. Detects Blueprint vs native UObject
	 * and returns the appropriate object for property access.
	 * For Blueprints: returns GeneratedClass->GetDefaultObject()
	 * For native assets: returns the loaded UObject directly
	 * @param AssetPath - Unreal asset path (e.g. /Game/Path/To/Asset)
	 * @param OutError - Populated on failure
	 * @return The UObject ready for property access, or nullptr on failure
	 */
	CLAIREON_API UObject* LoadAssetForEditing(const FString& AssetPath, FString& OutError);

	/**
	 * Resolve the on-disk filename a package should be saved to, choosing the extension from
	 * the package rather than assuming.
	 *
	 * Every Claireon save path goes through this, so all of them inherit the map-awareness. A
	 * package records no extension internally and name-based resolution searches .uasset before
	 * .umap, so hardcoding the asset extension silently writes a level to a shadow .uasset that
	 * then wins every later lookup -- see the implementation for the full mechanism.
	 *
	 * Refuses, rather than proceeding, when a file resolved by package name carries the wrong
	 * extension for what the package is: that condition means a shadow already exists, and
	 * saving into it deepens a corruption whose symptoms all read as success.
	 *
	 * Exposed for the specs, which need to exercise both branches without touching disk.
	 *
	 * @param Package     - The package about to be saved
	 * @param OutFileName - Populated with the target filename on success; cleared on refusal
	 * @param OutError    - Populated on failure
	 */
	CLAIREON_API bool ResolvePackageSaveFilename(const UPackage* Package, FString& OutFileName, FString& OutError);

	/**
	 * Find assets of a given class via AssetRegistry.
	 * @param Class - The UClass to search for
	 * @param NameFilter - Optional substring filter on asset name
	 * @param Limit - Maximum results to return (0 = unlimited)
	 * @return Array of matching FAssetData
	 */
	CLAIREON_API TArray<FAssetData> FindAssetsByClass(UClass* Class, const FString& NameFilter = TEXT(""), int32 Limit = 0);

	/**
	 * Find all C++ and Blueprint subclasses of a base class.
	 * @param BaseClass - The base UClass
	 * @param bIncludeAbstract - Whether to include abstract classes
	 * @param NameFilter - Optional substring filter on class name
	 * @return Array of derived UClass pointers
	 */
	CLAIREON_API TArray<UClass*> FindDerivedClasses(UClass* BaseClass, bool bIncludeAbstract = false, const FString& NameFilter = TEXT(""));

	/**
	 * Save a modified asset to disk. Handles both Blueprint and native assets.
	 * @param Asset - The UObject to save (must have been loaded via LoadAssetForEditing)
	 * @param OutError - Populated on failure
	 * @return true on success
	 */
	CLAIREON_API bool SaveAsset(UObject* Asset, FString& OutError);

	/**
	 * Assert that an asset's inner UObject name matches its containing
	 * package's short-name. Returns true on match; on mismatch populates
	 * OutError with package_path, package_short_name, inner_name,
	 * asset_class context so the caller's failure path can surface the
	 * same fields the asset_check_inner_name_invariant audit tool
	 * reports. See PROPOSAL.md "Creation-path assertion".
	 * @param Asset - The freshly-created asset (must be non-null and
	 *   have a valid outer package)
	 * @param OutError - Populated on mismatch with a human-readable
	 *   error message
	 * @return true if invariant holds, false on mismatch or invalid input
	 */
	CLAIREON_API bool AssertInnerNameMatchesPackage(const UObject* Asset, FString& OutError);

	/**
	 * Convert an FAssetData to a JSON object for tool responses.
	 * @param Data - The asset data to convert
	 * @return JSON object with path, name, class fields
	 */
	CLAIREON_API TSharedPtr<FJsonObject> AssetDataToJson(const FAssetData& Data);

	/**
	 * Refresh an open asset editor by closing and reopening it.
	 * No-op if the asset editor is not currently open -- avoids popping up editors for background edits.
	 * @param Asset - The asset whose editor should be refreshed
	 */
	CLAIREON_API void RefreshAssetEditorIfOpen(UObject* Asset);

	/**
	 * Emit a structured session-use hint at consecutive asset_path calls 6, 11, 16, and so on.
	 * Otherwise reset OutHint and leave ResponseData unchanged. Do not append hints to JSON summaries.
	 *
	 * @param ResponseData Receives session_hint when emitted
	 * @param ConsecutiveAssetPathCalls Calls using asset_path without session_id
	 * @param AssetPath Asset path shown in the hint
	 * @param SessionId Session ID shown in the hint
	 * @param ToolName Registered tool name for the hint
	 * @param OutHint Receives the hint, or null when none is emitted
	 */
	CLAIREON_API void EmitSessionHintIfNeeded(
		TSharedPtr<FJsonObject>& ResponseData,
		int32 ConsecutiveAssetPathCalls,
		const FString& AssetPath,
		const FString& SessionId,
		const FString& ToolName,
		TSharedPtr<FJsonObject>& OutHint);

	// Resolve a UClass by name, accepting either the "U"/"A"-prefixed or unprefixed
	// form (UClass::GetName() omits the prefix). Returns nullptr if no match.
	CLAIREON_API UClass* ResolveClassName(const FString& ClassName);

	/**
	 * Evict any in-memory UObject occupying AssetName within Package, moving it into the
	 * transient package under a FRESH unique name, cleared of public/standalone flags and
	 * marked garbage.
	 *
	 * The "recreate asset in place" idiom deletes the .uasset on disk before calling a
	 * create/duplicate API, but that does NOT remove an object already loaded in memory
	 * (e.g. from an earlier create this editor session). On UE 5.8
	 * FKismetEditorUtilities::CreateBlueprint and StaticDuplicateObject assert
	 * FindObject(Outer, Name) == nullptr, so callers must clear the slot first.
	 *
	 * The evicted object is deliberately NOT left under its original name: two evictions
	 * of the same name in one process would then collide in the transient package, and
	 * UObject::Rename treats that as a fatal error. For a Blueprint the collision lands on
	 * the derived generated-class name ("<Name>_C"), so the chosen name is checked for that
	 * too. Callers must therefore not assume the evicted object's name is preserved.
	 *
	 * No-op if Package is null or the name is free.
	 */
	CLAIREON_API void EvictInMemoryObject(UPackage* Package, const FString& AssetName);
}
