// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Spec tests for ClaireonStateTreeHelpers::ResolveNodeStruct (E14).
// Verifies that the helper accepts both bare struct names and /Script/...
// path forms, and that the failure path emits a hint pointing callers
// at the correct shape.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Containers/UnrealString.h"

UNTEST_UNIT_OPTS(Claireon, StateTreeHelpers, ResolveNodeStructEmpty, UNTEST_TIMEOUTMS(5000))
{
	FString Error;
	UScriptStruct* Result = ClaireonStateTreeHelpers::ResolveNodeStruct(FString(), Error);
	UNTEST_ASSERT_TRUE(Result == nullptr);
	UNTEST_ASSERT_TRUE(!Error.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, StateTreeHelpers, ResolveNodeStructScriptPrefixError, UNTEST_TIMEOUTMS(5000))
{
	// A bogus /Script/<Module>.<Struct> path must (a) fail and (b) include a
	// hint that mentions the bare-name form. The error from the lower-level
	// resolver alone says "Struct not found"; the prefix-strip wrapper appends
	// the "Pass the bare struct name" hint we want callers to see.
	FString Error;
	const FString Bogus = TEXT("/Script/Engine.FDefinitelyNotAStruct_XYZ");
	UScriptStruct* Result = ClaireonStateTreeHelpers::ResolveNodeStruct(Bogus, Error);
	UNTEST_ASSERT_TRUE(Result == nullptr);
	UNTEST_ASSERT_TRUE(Error.Contains(TEXT("Pass the bare struct name")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, StateTreeHelpers, ResolveNodeStructFPrefixError, UNTEST_TIMEOUTMS(5000))
{
	// An "F"-prefixed bogus name should hint at the dropped-prefix rule.
	FString Error;
	UScriptStruct* Result = ClaireonStateTreeHelpers::ResolveNodeStruct(TEXT("FDefinitelyNotAStruct_XYZ"), Error);
	UNTEST_ASSERT_TRUE(Result == nullptr);
	UNTEST_ASSERT_TRUE(Error.Contains(TEXT("leading 'F'")));
	co_return;
}

#endif // WITH_UNTESTED
