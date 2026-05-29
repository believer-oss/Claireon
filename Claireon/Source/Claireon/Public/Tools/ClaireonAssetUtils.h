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

	/**
	 * Refresh an open asset editor by closing and reopening it.
	 * No-op if the asset editor is not currently open -- avoids popping up editors for background edits.
	 * @param Asset - The asset whose editor should be refreshed
	 */
	CLAIREON_API void RefreshAssetEditorIfOpen(UObject* Asset);

	/**
	 * Open the asset editor for the given asset if running in the editor and not in a commandlet.
	 * Unconditionally calls OpenEditorForAsset -- call this when a session is first opened so the
	 * human watching the editor sees the asset surface. Guards against null Asset and null GEditor.
	 * @param Asset - The asset to open (e.g. the loaded UBlueprint, UBehaviorTree, etc.)
	 */
	CLAIREON_API void OpenAssetEditorIfHeadless(UObject* Asset);

	/**
	 * Emit a session-use hint into ResponseData and OutSummaryTag when the caller has made
	 * too many consecutive asset_path calls without reusing a session_id.
	 *
	 * Fires when ConsecutiveAssetPathCalls > 5 AND ConsecutiveAssetPathCalls % 5 == 1
	 * (first hint at call 6, then 11, 16, ...). On fire: populates ResponseData["session_hint"]
	 * with a prose nudge and sets OutSummaryTag to a short summary suffix. On no-fire:
	 * OutSummaryTag is set to empty string and ResponseData is not modified.
	 *
	 * @param ResponseData              - JSON response object; receives "session_hint" field on fire
	 * @param ConsecutiveAssetPathCalls - Counter tracking how many times asset_path was used without session_id
	 * @param AssetPath                 - Human-readable asset path shown in the hint text
	 * @param SessionId                 - Session ID shown in the hint text
	 * @param OutSummaryTag             - Receives the "\n\n[hint] ..." suffix on fire, empty string otherwise
	 */
	CLAIREON_API void EmitSessionHintIfNeeded(
		TSharedPtr<FJsonObject>& ResponseData,
		int32 ConsecutiveAssetPathCalls,
		const FString& AssetPath,
		const FString& SessionId,
		FString& OutSummaryTag);

	// Resolve a UClass by name, accepting either the "U"/"A"-prefixed or unprefixed
	// form (UClass::GetName() omits the prefix). Returns nullptr if no match.
	CLAIREON_API UClass* ResolveClassName(const FString& ClassName);
}
