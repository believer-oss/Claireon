// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonTool_SearchInBlueprints.h"
#include "Tools/ClaireonTool_SearchInBlueprintsIndexStatus.h"
#include "Dom/JsonObject.h"
#include "Templates/Function.h"

// ---------------------------------------------------------------------------
// Declarations of the testable wait/guard helpers defined in
// Private/Tools/ClaireonTool_SearchInBlueprints.cpp (same module; external
// linkage on purpose so no header is needed). Keep in sync with the
// definitions there.
// ---------------------------------------------------------------------------
namespace ClaireonSearchWait
{
	bool WaitForSearchWithHardTimeout(
		double TimeoutSeconds,
		const TFunction<bool()>& IsComplete,
		const TFunction<double()>& GetTimeSeconds,
		const TFunction<void()>& PumpIndexing,
		const TFunction<void()>& Yield,
		const TFunction<void()>& StopAndDrain,
		double& OutElapsedSeconds);

	bool TryAcquireSearchInFlightGuard();
	void ReleaseSearchInFlightGuard();
	bool IsSearchInFlight();

	void MarkSearchCompletedThisSession();
	bool HasAnySearchCompletedThisSession();
	void ResetSearchCompletedThisSessionForTests();
}

// ===========================================================================
// Hard-timeout wait loop (injected clock / search stub)
// ===========================================================================

/**
 * A never-completing search stub with a simulated clock must:
 * - return false within an epsilon of the requested timeout (simulated time),
 * - call StopAndDrain exactly once,
 * - run exactly ONE wait cycle (no hidden retry doubling the wall time),
 * - pump indexing once per poll iteration so FiB deferred indexing can
 *   progress while the game thread is blocked in the wait.
 */
UNTEST_UNIT_OPTS(Claireon, SearchTimeoutContract, HardTimeoutStopsAndDrainsWithoutRetry, UNTEST_TIMEOUTMS(5000))
{
	double SimTimeSeconds = 0.0;
	int32 IsCompleteCalls = 0;
	int32 PumpCalls = 0;
	int32 YieldCalls = 0;
	int32 StopCalls = 0;
	double ElapsedSeconds = -1.0;

	const bool bCompleted = ClaireonSearchWait::WaitForSearchWithHardTimeout(
		/*TimeoutSeconds*/ 2.0,
		/*IsComplete*/ [&IsCompleteCalls]() { ++IsCompleteCalls; return false; },
		/*GetTimeSeconds*/ [&SimTimeSeconds]() { return SimTimeSeconds; },
		/*PumpIndexing*/ [&PumpCalls]() { ++PumpCalls; },
		/*Yield*/ [&SimTimeSeconds, &YieldCalls]() { SimTimeSeconds += 0.01; ++YieldCalls; },
		/*StopAndDrain*/ [&StopCalls]() { ++StopCalls; },
		ElapsedSeconds);

	UNTEST_ASSERT_FALSE(bCompleted);

	// Stop/drain invoked exactly once, on expiry.
	UNTEST_EXPECT_EQ(StopCalls, 1);

	// Timeout honored within a small epsilon of simulated time.
	UNTEST_ASSERT_TRUE(ElapsedSeconds >= 2.0);
	UNTEST_ASSERT_TRUE(ElapsedSeconds <= 2.05);

	// Exactly one wait cycle: ~200 iterations at the 0.01s simulated step.
	// A hidden retry (the old behavior) would roughly double this.
	UNTEST_ASSERT_TRUE(YieldCalls >= 195);
	UNTEST_ASSERT_TRUE(YieldCalls <= 205);

	// Indexing pumped every poll iteration.
	UNTEST_EXPECT_EQ(PumpCalls, YieldCalls);
	UNTEST_ASSERT_TRUE(IsCompleteCalls >= YieldCalls);

	co_return;
}

/** A search that completes mid-wait returns true and never calls StopAndDrain. */
UNTEST_UNIT_OPTS(Claireon, SearchTimeoutContract, CompletionBeforeTimeoutSkipsStop, UNTEST_TIMEOUTMS(5000))
{
	double SimTimeSeconds = 0.0;
	int32 IsCompleteCalls = 0;
	int32 StopCalls = 0;
	double ElapsedSeconds = -1.0;

	const bool bCompleted = ClaireonSearchWait::WaitForSearchWithHardTimeout(
		/*TimeoutSeconds*/ 2.0,
		/*IsComplete*/ [&IsCompleteCalls]() { ++IsCompleteCalls; return IsCompleteCalls > 5; },
		/*GetTimeSeconds*/ [&SimTimeSeconds]() { return SimTimeSeconds; },
		/*PumpIndexing*/ []() {},
		/*Yield*/ [&SimTimeSeconds]() { SimTimeSeconds += 0.01; },
		/*StopAndDrain*/ [&StopCalls]() { ++StopCalls; },
		ElapsedSeconds);

	UNTEST_ASSERT_TRUE(bCompleted);
	UNTEST_EXPECT_EQ(StopCalls, 0);
	UNTEST_ASSERT_TRUE(ElapsedSeconds >= 0.0);
	UNTEST_ASSERT_TRUE(ElapsedSeconds < 2.0);

	co_return;
}

/** An already-complete search returns immediately without pumping or yielding. */
UNTEST_UNIT_OPTS(Claireon, SearchTimeoutContract, AlreadyCompleteReturnsImmediately, UNTEST_TIMEOUTMS(5000))
{
	double SimTimeSeconds = 0.0;
	int32 PumpCalls = 0;
	int32 YieldCalls = 0;
	int32 StopCalls = 0;
	double ElapsedSeconds = -1.0;

	const bool bCompleted = ClaireonSearchWait::WaitForSearchWithHardTimeout(
		/*TimeoutSeconds*/ 2.0,
		/*IsComplete*/ []() { return true; },
		/*GetTimeSeconds*/ [&SimTimeSeconds]() { return SimTimeSeconds; },
		/*PumpIndexing*/ [&PumpCalls]() { ++PumpCalls; },
		/*Yield*/ [&SimTimeSeconds, &YieldCalls]() { SimTimeSeconds += 0.01; ++YieldCalls; },
		/*StopAndDrain*/ [&StopCalls]() { ++StopCalls; },
		ElapsedSeconds);

	UNTEST_ASSERT_TRUE(bCompleted);
	UNTEST_EXPECT_EQ(PumpCalls, 0);
	UNTEST_EXPECT_EQ(YieldCalls, 0);
	UNTEST_EXPECT_EQ(StopCalls, 0);
	UNTEST_EXPECT_EQ(ElapsedSeconds, 0.0);

	co_return;
}

// ===========================================================================
// Reentrancy (overlap) guard
// ===========================================================================

/** The in-flight guard is exclusive and re-acquirable after release. */
UNTEST_UNIT_OPTS(Claireon, SearchTimeoutContract, InFlightGuardIsExclusive, UNTEST_TIMEOUTMS(5000))
{
	// Precondition: no search in flight when the test starts.
	UNTEST_ASSERT_FALSE(ClaireonSearchWait::IsSearchInFlight());

	UNTEST_ASSERT_TRUE(ClaireonSearchWait::TryAcquireSearchInFlightGuard());
	UNTEST_EXPECT_TRUE(ClaireonSearchWait::IsSearchInFlight());

	// Second acquisition while held must fail.
	const bool bSecondAcquire = ClaireonSearchWait::TryAcquireSearchInFlightGuard();

	ClaireonSearchWait::ReleaseSearchInFlightGuard();

	UNTEST_ASSERT_FALSE(bSecondAcquire);
	UNTEST_ASSERT_FALSE(ClaireonSearchWait::IsSearchInFlight());

	// Re-acquirable after release.
	UNTEST_ASSERT_TRUE(ClaireonSearchWait::TryAcquireSearchInFlightGuard());
	ClaireonSearchWait::ReleaseSearchInFlightGuard();

	co_return;
}

/**
 * A second concurrent bp_search invocation returns the busy error immediately
 * (exact error text contract) without starting a stream search.
 */
UNTEST_UNIT_OPTS(Claireon, SearchTimeoutContract, ExecuteReturnsBusyWhileSearchInFlight, UNTEST_TIMEOUTMS(5000))
{
	// Simulate an in-flight search by holding the guard.
	UNTEST_ASSERT_TRUE(ClaireonSearchWait::TryAcquireSearchInFlightGuard());

	ClaireonTool_SearchInBlueprints Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("query"), TEXT("ClaireonOverlapGuardProbe"));

	IClaireonTool::FToolResult Result = Tool.Execute(Args);

	// Release before asserting so a failed assert cannot leak the guard.
	ClaireonSearchWait::ReleaseSearchInFlightGuard();

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_STREQ(*Result.ErrorMessage,
		TEXT("bp_search is busy: another Blueprint search is already in flight. Concurrent searches are not supported; wait for the in-flight search to complete or time out, then retry."));

	co_return;
}

/** Parameter validation still precedes the busy path for the working call shape. */
UNTEST_UNIT_OPTS(Claireon, SearchTimeoutContract, MissingQueryErrorsBeforeGuard, UNTEST_TIMEOUTMS(5000))
{
	UNTEST_ASSERT_TRUE(ClaireonSearchWait::TryAcquireSearchInFlightGuard());

	ClaireonTool_SearchInBlueprints Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();

	IClaireonTool::FToolResult Result = Tool.Execute(Args);

	ClaireonSearchWait::ReleaseSearchInFlightGuard();

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_STREQ(*Result.ErrorMessage, TEXT("Missing required parameter: query"));

	co_return;
}

// ===========================================================================
// index_status: first-search cost reporting
// ===========================================================================

/**
 * Until a bp_search completes this session, index_status must report
 * first_search_may_index_full_corpus=true; after one completes, false.
 */
UNTEST_UNIT_OPTS(Claireon, SearchTimeoutContract, IndexStatusReportsFirstSearchCorpusFlag, UNTEST_TIMEOUTMS(30000))
{
	const bool bHadCompletedSearch = ClaireonSearchWait::HasAnySearchCompletedThisSession();
	ClaireonSearchWait::ResetSearchCompletedThisSessionForTests();

	ClaireonTool_SearchInBlueprintsIndexStatus StatusTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();

	IClaireonTool::FToolResult FirstResult = StatusTool.Execute(Args);
	bool bFlagBefore = false;
	const bool bHasFlagBefore = FirstResult.Data.IsValid()
		&& FirstResult.Data->TryGetBoolField(TEXT("first_search_may_index_full_corpus"), bFlagBefore);

	ClaireonSearchWait::MarkSearchCompletedThisSession();

	IClaireonTool::FToolResult SecondResult = StatusTool.Execute(Args);
	bool bFlagAfter = true;
	const bool bHasFlagAfter = SecondResult.Data.IsValid()
		&& SecondResult.Data->TryGetBoolField(TEXT("first_search_may_index_full_corpus"), bFlagAfter);

	// Restore whatever state the session had before this test ran.
	if (!bHadCompletedSearch)
	{
		ClaireonSearchWait::ResetSearchCompletedThisSessionForTests();
	}

	UNTEST_ASSERT_FALSE(FirstResult.bIsError);
	UNTEST_ASSERT_TRUE(bHasFlagBefore);
	UNTEST_ASSERT_TRUE(bFlagBefore);

	UNTEST_ASSERT_FALSE(SecondResult.bIsError);
	UNTEST_ASSERT_TRUE(bHasFlagAfter);
	UNTEST_ASSERT_FALSE(bFlagAfter);

	co_return;
}

#endif // WITH_UNTESTED
