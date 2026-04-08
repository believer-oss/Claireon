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
	 * Convert an FAssetData to a JSON object for tool responses.
	 * @param Data - The asset data to convert
	 * @return JSON object with path, name, class fields
	 */
	CLAIREON_API TSharedPtr<FJsonObject> AssetDataToJson(const FAssetData& Data);
}
