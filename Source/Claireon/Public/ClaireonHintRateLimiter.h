// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

/**
 * Game-thread-only hint limiter keyed by (HintKey, RecipientId).
 * Scope resets per top-level python_execute request; direct tool calls bypass the limiter.
 * RecipientId currently shares the "default" bucket. Do not widen scope to a session
 * until conversation identity is available, or one caller could suppress another's hints.
 */
namespace ClaireonHintRateLimiter
{
	/** True exactly once per (Key, RecipientId) within the current scope.
	 *  Keyless hints (Key == NAME_None) are never limited: always true. */
	CLAIREON_API bool ShouldEmit(FName Key, const FString& RecipientId);

	/** Clear the scope. Called at the start of every top-level python_execute invocation
	 *  (FClaireonBridgeInvocationScope constructor) and by tests. */
	CLAIREON_API void ResetScope();

	/**
	 * Replace scope state and return the previous state. Nested python_execute calls need
	 * fresh state and must restore the outer state so their drains do not consume outer hints.
	 */
	CLAIREON_API TSet<FString> ExchangeScopeState(TSet<FString> NewState);
}
