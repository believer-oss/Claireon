// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

// Editor fixtures use the gitignored /Game/_ClaireonBPEditor root. Unique paths
// isolate per-test fixtures; fixed paths preserve an open editor across calls so
// BlueprintAssist next-tick initialization can complete.

#include "CoreMinimal.h"

#if WITH_CLAIREON_TESTS && WITH_EDITOR

class UBlueprint;
class UEdGraph;
class UObject;

namespace ClaireonBPEditorFixtures
{
	/** Gitignored scratch root. Nothing here is ever committed. */
	const TCHAR* Root();

	/** Unique per invocation, so a crashed run cannot poison the next one. */
	FString UniquePath(const TCHAR* Stem);

	/**
	 * Stable across runs, for a fixture whose asset editor must survive a call boundary.
	 * See the two-fixture-shapes note above.
	 */
	FString FixedPath(const TCHAR* Stem);

	/**
	 * Share one graph across BlueprintAssist tests in a test_run call. Only the active
	 * graph handler ticks, and switching handlers requires a next-tick timer that
	 * cannot run while the tool holds the game thread.
	 */
	FString SharedBlueprintAssistFixturePath();

	/**
	 * Resolve without loading. Loading and opening an editor in the same frame can
	 * assert during Blueprint editor library loading.
	 */
	UObject* Resolve(const FString& AssetPath);

	/**
	 * Create through bp_create; return null with OutError on failure. This replaces
	 * an existing asset, so fixed-path tests must check Resolve first.
	 */
	UBlueprint* Create(const FString& AssetPath, FString& OutError);

	/**
	 * Save without dialogs. Persist fixed fixtures to avoid a restore-packages modal
	 * blocking the next automated editor launch.
	 */
	bool Save(UBlueprint* Blueprint);

	/** Release the session and delete through ClaireonTestAssetDeletion. */
	void Teardown(const FString& AssetPath);

	/** First valid ubergraph page, or null. */
	UEdGraph* FirstUbergraph(UBlueprint* Blueprint);
}

#endif // WITH_CLAIREON_TESTS && WITH_EDITOR
