// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

/** Why the editor world is not ready. */
enum class EClaireonWorldNotReadyReason : uint8
{
	Ready,					// World is available
	NoEditor,				// GEditor is null
	NoWorld,				// GEditor exists but GetEditorWorldContext().World() is null
	WorldTearingDown,		// World exists but bIsTearingDown is set
	DeferredWorldTransition // A LoadMap/DuplicateAndOpenMap/PIEStart/PIEStop is queued
};

/** Result of a world-readiness check. */
struct FClaireonWorldReadinessResult
{
	bool bReady = false;
	EClaireonWorldNotReadyReason Reason = EClaireonWorldNotReadyReason::Ready;
	FString Message;	  // User-facing error, e.g. "No world loaded."
	FString RecoveryHint; // Actionable fix, e.g. "Use open_map to load a map first."
};

struct CLAIREON_API FClaireonWorldReadiness
{
	/** Check whether the editor world is available and stable. */
	static FClaireonWorldReadinessResult Check();

	/** Convenience: returns true if the world is ready. */
	static bool IsReady();

	FClaireonWorldReadiness() = delete; // No instantiation
};
