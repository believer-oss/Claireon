// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Limit each key/recipient pair within one request; reset for the next request.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonHintRateLimiter.h"

UNTEST_UNIT(Claireon, HintRateLimiter, HintRateLimiter_OncePerKeyRecipientPerScope)
{
	ClaireonHintRateLimiter::ResetScope();

	const FName Key(TEXT("claireon.lint.judgement-reference"));
	UNTEST_EXPECT_TRUE(ClaireonHintRateLimiter::ShouldEmit(Key, TEXT("default")));
	UNTEST_EXPECT_FALSE(ClaireonHintRateLimiter::ShouldEmit(Key, TEXT("default")));
	UNTEST_EXPECT_FALSE(ClaireonHintRateLimiter::ShouldEmit(Key, TEXT("default")));

	UNTEST_EXPECT_TRUE(ClaireonHintRateLimiter::ShouldEmit(FName(TEXT("log_category_filter_excluded")), TEXT("default")));
	co_return;
}

UNTEST_UNIT(Claireon, HintRateLimiter, HintRateLimiter_KeylessNeverLimited)
{
	ClaireonHintRateLimiter::ResetScope();

	UNTEST_EXPECT_TRUE(ClaireonHintRateLimiter::ShouldEmit(NAME_None, TEXT("default")));
	UNTEST_EXPECT_TRUE(ClaireonHintRateLimiter::ShouldEmit(NAME_None, TEXT("default")));
	UNTEST_EXPECT_TRUE(ClaireonHintRateLimiter::ShouldEmit(NAME_None, TEXT("default")));
	co_return;
}

UNTEST_UNIT(Claireon, HintRateLimiter, HintRateLimiter_ResetScopeReopensEveryKey)
{
	ClaireonHintRateLimiter::ResetScope();

	const FName Key(TEXT("claireon.lint.judgement-reference"));
	UNTEST_EXPECT_TRUE(ClaireonHintRateLimiter::ShouldEmit(Key, TEXT("default")));
	UNTEST_EXPECT_FALSE(ClaireonHintRateLimiter::ShouldEmit(Key, TEXT("default")));

	ClaireonHintRateLimiter::ResetScope();
	UNTEST_EXPECT_TRUE(ClaireonHintRateLimiter::ShouldEmit(Key, TEXT("default")));
	co_return;
}

UNTEST_UNIT(Claireon, HintRateLimiter, HintRateLimiter_DistinctRecipientsIndependent)
{
	ClaireonHintRateLimiter::ResetScope();

	const FName Key(TEXT("claireon.lint.judgement-reference"));
	UNTEST_EXPECT_TRUE(ClaireonHintRateLimiter::ShouldEmit(Key, TEXT("agent-a")));
	UNTEST_EXPECT_FALSE(ClaireonHintRateLimiter::ShouldEmit(Key, TEXT("agent-a")));
	UNTEST_EXPECT_TRUE(ClaireonHintRateLimiter::ShouldEmit(Key, TEXT("agent-b")));
	UNTEST_EXPECT_FALSE(ClaireonHintRateLimiter::ShouldEmit(Key, TEXT("agent-b")));

	ClaireonHintRateLimiter::ResetScope();
	co_return;
}

#endif // WITH_UNTESTED
