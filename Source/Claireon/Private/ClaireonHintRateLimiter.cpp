// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonHintRateLimiter.h"

namespace ClaireonHintRateLimiterInternal
{
	// Game-thread only. Hint keys must not contain the delimiter "|".
	static TSet<FString> ClHRL_EmittedThisScope;
}

bool ClaireonHintRateLimiter::ShouldEmit(FName Key, const FString& RecipientId)
{
	if (Key.IsNone())
	{
		return true;
	}

	const FString ScopeKey = Key.ToString() + TEXT("|") + RecipientId;
	if (ClaireonHintRateLimiterInternal::ClHRL_EmittedThisScope.Contains(ScopeKey))
	{
		return false;
	}
	ClaireonHintRateLimiterInternal::ClHRL_EmittedThisScope.Add(ScopeKey);
	return true;
}

void ClaireonHintRateLimiter::ResetScope()
{
	ClaireonHintRateLimiterInternal::ClHRL_EmittedThisScope.Reset();
}

TSet<FString> ClaireonHintRateLimiter::ExchangeScopeState(TSet<FString> NewState)
{
	TSet<FString> Previous = MoveTemp(ClaireonHintRateLimiterInternal::ClHRL_EmittedThisScope);
	ClaireonHintRateLimiterInternal::ClHRL_EmittedThisScope = MoveTemp(NewState);
	return Previous;
}
