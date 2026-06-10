// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

/**
 * Static utility that saves all dirty packages before crash-risk operations
 * (Python execution, deferred world-transition actions).
 *
 * Controlled by UClaireonSettings auto-save properties. A crash flag prevents
 * saving corrupted state after bridge-level Python failures.
 */
class FClaireonAutoSave
{
public:
	/**
	 * Save all dirty packages if auto-save is enabled and the debounce
	 * interval has elapsed. No-ops during PIE or when the crash flag is set.
	 *
	 * @param bIsPythonExecution  true when called before Python execution
	 *                            (checks bAutoSaveBeforePythonExecution setting).
	 *                            false when called before deferred actions
	 *                            (checks bAutoSaveBeforeDeferredActions setting).
	 * @return Number of packages saved, or 0 if skipped.
	 */
	static int32 SaveIfNeeded(bool bIsPythonExecution);

	/** Set the crash flag -- suppresses auto-save until cleared. */
	static void SetCrashFlag();

	/** Clear the crash flag (e.g. on next successful execution). */
	static void ClearCrashFlag();

	/** Returns true if the crash flag is currently set. */
	static bool IsCrashFlagSet();

private:
	/** Timestamp of the last successful auto-save (for debounce). */
	static double LastSaveTimeSeconds;

	/** When true, auto-save is suppressed to avoid persisting corrupted state. */
	static bool bCrashFlag;
};
