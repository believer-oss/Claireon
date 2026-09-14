// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Check configuration gating and armed/disarmed behavior. The header also asserts
// that fault injection cannot be enabled in Shipping builds.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonBPSnapshot.h"
#include "Tools/ClaireonBPMutationResult.h"

#if WITH_CLAIREON_TESTS
static_assert(!UE_BUILD_SHIPPING,
	"WITH_CLAIREON_TESTS is set in a shipping build; the fault-injection seam would ship.");
#endif


UNTEST_UNIT(Claireon, BPFaultSeam, SeamIsGuardedAndCompilesOutOfShippingBuilds)
{
	const bool bExpectedCompiledIn =
		(UE_BUILD_SHIPPING == 0) && (UE_BUILD_TEST == 0) && (WITH_UNTESTED == 1);
	UNTEST_EXPECT_EQ(ClaireonBPFaultInjection::IsCompiledIn(), bExpectedCompiledIn);

	// The guard-off macro must not reference fault-injection symbols.
	UNTEST_EXPECT_FALSE(CLAIREON_BP_SHOULD_INJECT_FAILURE(
		ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::Collapse)));

#if WITH_CLAIREON_TESTS
	UNTEST_EXPECT_EQ(static_cast<int32>(UE_BUILD_SHIPPING), 0);

	{
		const ClaireonBPFaultInjection::FScopedFault Fault(
			ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::Collapse));

		UNTEST_EXPECT_TRUE(CLAIREON_BP_SHOULD_INJECT_FAILURE(
			ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::Collapse)));
		UNTEST_EXPECT_FALSE(CLAIREON_BP_SHOULD_INJECT_FAILURE(
			ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::Rename)));
		UNTEST_EXPECT_FALSE(CLAIREON_BP_SHOULD_INJECT_FAILURE(
			ClaireonBPMutation::ToWireString(EClaireonBPSetterPhase::FlagModification)));
		UNTEST_EXPECT_FALSE(CLAIREON_BP_SHOULD_INJECT_FAILURE(
			ClaireonBPMutation::ToWireString(EClaireonBPFormatPhase::Settle)));
	}

	UNTEST_EXPECT_TRUE(ClaireonBPFaultInjection::GetArmedPhase().IsEmpty());
	UNTEST_EXPECT_FALSE(CLAIREON_BP_SHOULD_INJECT_FAILURE(
		ClaireonBPMutation::ToWireString(EClaireonBPExtractionPhase::Collapse)));
#endif

	co_return;
}

#endif // WITH_UNTESTED
