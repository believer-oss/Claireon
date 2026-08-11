// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// WI-8 contract tests for the non-blocking wait design behind pie_wait_for /
// pie_wait_poll and the test.run deadline policy.
//
// The old pie_wait_for sleep-polled on the game thread, starving the very
// ticks its conditions needed to become true, and test.run's noTimeout=true
// resolved the deadline to numeric max (an unbounded game-thread block).
// These tests pin the replacement behavior:
//   - an already-true condition completes immediately with no wait registered;
//   - a never-true condition yields a wait that can be polled to a timedOut
//     terminal state on a simulated clock, without sleeping the test thread;
//   - noTimeout resolves to the documented 3600s cap, never numeric max.
//
// The registry tests inject a fake clock (no editor, no PIE, no sleeping).
// The tool-level tests drive Execute with FJsonObject payloads and assert
// the exact error text contract on failure paths.

#if WITH_UNTESTED

#include "Untest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Math/NumericLimits.h"
#include "Math/UnrealMathUtility.h"
#include "Tools/ClaireonTool_PIEWaitFor.h"
#include "Tools/ClaireonTool_PIEWaitPoll.h"
#include "Tools/ClaireonTool_TestRun.h"
#include "Tools/ClaireonWaitSupport.h"
#include "Tools/IClaireonTool.h"

/**
 * An already-true condition must complete immediately: no wait registered,
 * no wait id handed out, nothing left tracked in the registry.
 */
UNTEST_UNIT_OPTS(Claireon, WaitToolContract, AlreadyTrueConditionImmediateSuccessNoWaitRegistered, UNTEST_TIMEOUTMS(5000))
{
	double FakeNowSeconds = 1000.0;
	FClaireonPIEWaitRegistry Registry([&FakeNowSeconds]() { return FakeNowSeconds; });

	const ClaireonWaitSupport::FStartWaitOutcome Outcome = ClaireonWaitSupport::StartOrCompleteWait(
		Registry,
		TEXT("alwaysTrue"),
		[]() { return true; },
		30.0,
		TFunction<FString()>());

	UNTEST_ASSERT_TRUE(Outcome.bImmediatelyMet);
	UNTEST_ASSERT_TRUE(Outcome.WaitId.IsEmpty());
	UNTEST_ASSERT_TRUE(Registry.NumTrackedWaits() == 0);
	UNTEST_ASSERT_TRUE(Registry.NumPendingWaits() == 0);

	co_return;
}

/**
 * A never-true condition must yield a 'waiting' outcome with a wait id.
 * Polling before the deadline reports Pending (and does NOT consume the
 * wait); polling after the simulated deadline reports TimedOut with the
 * wait's diagnostics -- even when no TickWaits ran in between -- and
 * consumes the record. The entire flow runs on the injected clock: the
 * test thread never sleeps out the 5-second timeout.
 */
UNTEST_UNIT_OPTS(Claireon, WaitToolContract, NeverTrueConditionWaitsThenPollReportsTimedOutOnSimulatedClock, UNTEST_TIMEOUTMS(5000))
{
	const double RealStartSeconds = FPlatformTime::Seconds();

	double FakeNowSeconds = 100.0;
	FClaireonPIEWaitRegistry Registry([&FakeNowSeconds]() { return FakeNowSeconds; });

	bool bDiagnosticsProviderRan = false;
	const ClaireonWaitSupport::FStartWaitOutcome Outcome = ClaireonWaitSupport::StartOrCompleteWait(
		Registry,
		TEXT("neverTrue"),
		[]() { return false; },
		5.0,
		[&bDiagnosticsProviderRan]()
		{
			bDiagnosticsProviderRan = true;
			return FString(TEXT("diagnostics.marker: wait-tool-contract\n"));
		});

	UNTEST_ASSERT_FALSE(Outcome.bImmediatelyMet);
	UNTEST_ASSERT_FALSE(Outcome.WaitId.IsEmpty());
	UNTEST_ASSERT_TRUE(Registry.NumPendingWaits() == 1);

	// Pre-deadline: a frame tick plus a poll must report Pending and keep the wait alive.
	FakeNowSeconds = 104.0;
	Registry.TickWaits();
	const FClaireonPIEWaitRegistry::FWaitStatus PendingStatus = Registry.Poll(Outcome.WaitId);
	UNTEST_ASSERT_TRUE(PendingStatus.bFound);
	UNTEST_ASSERT_TRUE(PendingStatus.State == FClaireonPIEWaitRegistry::EWaitState::Pending);
	UNTEST_ASSERT_TRUE(FMath::IsNearlyEqual(PendingStatus.ElapsedSeconds, 4.0, 0.001));
	UNTEST_ASSERT_FALSE(bDiagnosticsProviderRan);
	UNTEST_ASSERT_TRUE(Registry.NumPendingWaits() == 1);

	// Past the deadline: the poll itself must observe the timeout even with
	// no intervening TickWaits, run the diagnostics provider, and consume the
	// terminal record.
	FakeNowSeconds = 105.5;
	const FClaireonPIEWaitRegistry::FWaitStatus TerminalStatus = Registry.Poll(Outcome.WaitId);
	UNTEST_ASSERT_TRUE(TerminalStatus.bFound);
	UNTEST_ASSERT_TRUE(TerminalStatus.State == FClaireonPIEWaitRegistry::EWaitState::TimedOut);
	UNTEST_ASSERT_TRUE(FMath::IsNearlyEqual(TerminalStatus.ElapsedSeconds, 5.5, 0.001));
	UNTEST_ASSERT_TRUE(bDiagnosticsProviderRan);
	UNTEST_ASSERT_TRUE(TerminalStatus.TimeoutDiagnostics.Contains(TEXT("wait-tool-contract")));

	// Consumed: a second poll of the same id reports not-found.
	const FClaireonPIEWaitRegistry::FWaitStatus ConsumedStatus = Registry.Poll(Outcome.WaitId);
	UNTEST_ASSERT_FALSE(ConsumedStatus.bFound);
	UNTEST_ASSERT_TRUE(Registry.NumTrackedWaits() == 0);

	// The 5s (simulated) timeout must not have cost 5s of wall clock.
	UNTEST_ASSERT_TRUE(FPlatformTime::Seconds() - RealStartSeconds < 2.0);

	co_return;
}

/**
 * A condition that becomes true is marked Met by the frame tick, stays Met
 * (a later clock advance past the deadline must not flip it to TimedOut),
 * and reports the elapsed time at which it was met.
 */
UNTEST_UNIT_OPTS(Claireon, WaitToolContract, ConditionBecomingTrueIsMetNotTimedOut, UNTEST_TIMEOUTMS(5000))
{
	double FakeNowSeconds = 50.0;
	FClaireonPIEWaitRegistry Registry([&FakeNowSeconds]() { return FakeNowSeconds; });

	bool bConditionValue = false;
	const ClaireonWaitSupport::FStartWaitOutcome Outcome = ClaireonWaitSupport::StartOrCompleteWait(
		Registry,
		TEXT("flag"),
		[&bConditionValue]() { return bConditionValue; },
		30.0,
		TFunction<FString()>());

	UNTEST_ASSERT_FALSE(Outcome.bImmediatelyMet);
	UNTEST_ASSERT_TRUE(Registry.NumPendingWaits() == 1);

	// Flip the condition and advance one frame: the wait goes terminal (Met).
	FakeNowSeconds = 51.25;
	bConditionValue = true;
	Registry.TickWaits();
	UNTEST_ASSERT_TRUE(Registry.NumPendingWaits() == 0);
	UNTEST_ASSERT_TRUE(Registry.NumTrackedWaits() == 1);

	// Advance far past the deadline before polling: Met must not decay to TimedOut.
	FakeNowSeconds = 500.0;
	const FClaireonPIEWaitRegistry::FWaitStatus MetStatus = Registry.Poll(Outcome.WaitId);
	UNTEST_ASSERT_TRUE(MetStatus.bFound);
	UNTEST_ASSERT_TRUE(MetStatus.State == FClaireonPIEWaitRegistry::EWaitState::Met);
	UNTEST_ASSERT_TRUE(FMath::IsNearlyEqual(MetStatus.ElapsedSeconds, 1.25, 0.001));
	UNTEST_ASSERT_TRUE(MetStatus.TimeoutDiagnostics.IsEmpty());
	UNTEST_ASSERT_TRUE(Registry.NumTrackedWaits() == 0);

	co_return;
}

/**
 * test.run deadline policy: noTimeout=true resolves to the documented
 * 3600-second cap -- never TNumericLimits<double>::Max() -- and the default
 * remains 1800 seconds.
 */
UNTEST_UNIT_OPTS(Claireon, WaitToolContract, TestRunNoTimeoutResolvesToDocumentedCap, UNTEST_TIMEOUTMS(5000))
{
	const double DefaultTimeout = ClaireonWaitSupport::ResolveTestRunTimeoutSeconds(false);
	const double CappedTimeout = ClaireonWaitSupport::ResolveTestRunTimeoutSeconds(true);

	UNTEST_ASSERT_TRUE(DefaultTimeout == ClaireonWaitSupport::TestRunDefaultTimeoutSeconds);
	UNTEST_ASSERT_TRUE(DefaultTimeout == 1800.0);
	UNTEST_ASSERT_TRUE(CappedTimeout == ClaireonWaitSupport::TestRunNoTimeoutCapSeconds);
	UNTEST_ASSERT_TRUE(CappedTimeout == 3600.0);
	UNTEST_ASSERT_TRUE(CappedTimeout < TNumericLimits<double>::Max());

	co_return;
}

/**
 * pie_wait_for poll mode failure path: an unknown wait_id must return the
 * exact documented error, and must do so without requiring GEditor (the poll
 * path only touches the wait registry).
 */
UNTEST_UNIT_OPTS(Claireon, WaitToolContract, PIEWaitForPollUnknownWaitIdReturnsExactError, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_PIEWaitFor Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("wait_id"), TEXT("bogus-wait-id"));

	const IClaireonTool::FToolResult Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_STREQ(Result.ErrorMessage,
		TEXT("Unknown wait_id 'bogus-wait-id'. The wait was never started, or its terminal state was already consumed by a previous poll."));

	co_return;
}

/**
 * pie_wait_poll failure paths: a missing (or empty) wait_id must return the
 * exact documented error; an unknown wait_id must return the shared
 * unknown-id error.
 */
UNTEST_UNIT_OPTS(Claireon, WaitToolContract, PIEWaitPollMissingOrUnknownWaitIdReturnsExactError, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_PIEWaitPoll Tool;

	TSharedPtr<FJsonObject> EmptyArgs = MakeShared<FJsonObject>();
	const IClaireonTool::FToolResult MissingResult = Tool.Execute(EmptyArgs);
	UNTEST_ASSERT_TRUE(MissingResult.bIsError);
	UNTEST_ASSERT_STREQ(MissingResult.ErrorMessage, TEXT("Missing required parameter: wait_id"));

	TSharedPtr<FJsonObject> UnknownArgs = MakeShared<FJsonObject>();
	UnknownArgs->SetStringField(TEXT("wait_id"), TEXT("no-such-wait"));
	const IClaireonTool::FToolResult UnknownResult = Tool.Execute(UnknownArgs);
	UNTEST_ASSERT_TRUE(UnknownResult.bIsError);
	UNTEST_ASSERT_STREQ(UnknownResult.ErrorMessage,
		TEXT("Unknown wait_id 'no-such-wait'. The wait was never started, or its terminal state was already consumed by a previous poll."));

	co_return;
}

/**
 * Schema contract: pie_wait_for declares the wait_id poll parameter and no
 * longer hard-requires 'condition' at the schema level (it is conditionally
 * required by the implementation); pie_wait_poll requires wait_id; test.run
 * documents the 3600-second noTimeout cap and the 1800-second default.
 */
UNTEST_UNIT_OPTS(Claireon, WaitToolContract, SchemasDeclareWaitIdAndTimeoutCap, UNTEST_TIMEOUTMS(5000))
{
	// pie_wait_for
	ClaireonTool_PIEWaitFor WaitForTool;
	const TSharedPtr<FJsonObject> WaitForSchema = WaitForTool.GetInputSchema();
	UNTEST_ASSERT_TRUE(WaitForSchema.IsValid());
	const TSharedPtr<FJsonObject> WaitForProps = WaitForSchema->GetObjectField(TEXT("properties"));
	UNTEST_ASSERT_TRUE(WaitForProps.IsValid());
	UNTEST_ASSERT_TRUE(WaitForProps->HasField(TEXT("wait_id")));
	UNTEST_ASSERT_TRUE(WaitForProps->HasField(TEXT("condition")));
	UNTEST_ASSERT_TRUE(WaitForProps->HasField(TEXT("timeoutSeconds")));
	// Backward compatibility: pollIntervalMs is still declared (documented as ignored).
	UNTEST_ASSERT_TRUE(WaitForProps->HasField(TEXT("pollIntervalMs")));
	UNTEST_ASSERT_TRUE(WaitForSchema->GetArrayField(TEXT("required")).Num() == 0);

	// pie_wait_poll
	ClaireonTool_PIEWaitPoll WaitPollTool;
	const TSharedPtr<FJsonObject> WaitPollSchema = WaitPollTool.GetInputSchema();
	UNTEST_ASSERT_TRUE(WaitPollSchema.IsValid());
	const TSharedPtr<FJsonObject> WaitPollProps = WaitPollSchema->GetObjectField(TEXT("properties"));
	UNTEST_ASSERT_TRUE(WaitPollProps.IsValid());
	UNTEST_ASSERT_TRUE(WaitPollProps->HasField(TEXT("wait_id")));
	const TArray<TSharedPtr<FJsonValue>>& WaitPollRequired = WaitPollSchema->GetArrayField(TEXT("required"));
	UNTEST_ASSERT_TRUE(WaitPollRequired.Num() == 1);
	UNTEST_ASSERT_STREQ(WaitPollRequired[0]->AsString(), TEXT("wait_id"));

	// test.run
	ClaireonTool_TestRun TestRunTool;
	const TSharedPtr<FJsonObject> TestRunSchema = TestRunTool.GetInputSchema();
	UNTEST_ASSERT_TRUE(TestRunSchema.IsValid());
	const TSharedPtr<FJsonObject> TestRunProps = TestRunSchema->GetObjectField(TEXT("properties"));
	UNTEST_ASSERT_TRUE(TestRunProps.IsValid());
	const FString NoTimeoutDescription =
		TestRunProps->GetObjectField(TEXT("noTimeout"))->GetStringField(TEXT("description"));
	UNTEST_ASSERT_TRUE(NoTimeoutDescription.Contains(TEXT("3600")));
	UNTEST_ASSERT_TRUE(NoTimeoutDescription.Contains(TEXT("1800")));

	co_return;
}

/**
 * End-to-end poll translation: a wait started in the production singleton
 * registry must be pollable through BuildWaitPollResult with the documented
 * data envelope, both while waiting and after its (short) timeout expires.
 * Uses the singleton's real clock with a 0.1s timeout: the second poll runs
 * after a real >0.1s has elapsed, which requires no editor ticking because
 * Poll itself advances the deadline check.
 */
UNTEST_UNIT_OPTS(Claireon, WaitToolContract, SingletonRegistryPollEnvelopeRoundTrip, UNTEST_TIMEOUTMS(5000))
{
	FClaireonPIEWaitRegistry& Registry = FClaireonPIEWaitRegistry::Get();

	const FString WaitId = Registry.StartWait(
		TEXT("neverTrue"),
		[]() { return false; },
		0.1,
		[]() { return FString(TEXT("diagnostics.marker: singleton-roundtrip\n")); });
	UNTEST_ASSERT_FALSE(WaitId.IsEmpty());

	// Immediate poll: still waiting.
	const IClaireonTool::FToolResult WaitingResult =
		ClaireonWaitSupport::BuildWaitPollResult(Registry, WaitId);
	UNTEST_ASSERT_FALSE(WaitingResult.bIsError);
	UNTEST_ASSERT_TRUE(WaitingResult.Data.IsValid());
	UNTEST_ASSERT_STREQ(WaitingResult.Data->GetStringField(TEXT("status")), TEXT("waiting"));
	UNTEST_ASSERT_FALSE(WaitingResult.Data->GetBoolField(TEXT("timedOut")));

	// Let >0.1s of real time pass. This is the only real wait in the file and
	// it is intentionally tiny; the poll itself performs the deadline check.
	FPlatformProcess::Sleep(0.15f);

	const IClaireonTool::FToolResult TimedOutResult =
		ClaireonWaitSupport::BuildWaitPollResult(Registry, WaitId);
	UNTEST_ASSERT_FALSE(TimedOutResult.bIsError);
	UNTEST_ASSERT_TRUE(TimedOutResult.Data.IsValid());
	UNTEST_ASSERT_STREQ(TimedOutResult.Data->GetStringField(TEXT("status")), TEXT("timedOut"));
	UNTEST_ASSERT_FALSE(TimedOutResult.Data->GetBoolField(TEXT("conditionMet")));
	UNTEST_ASSERT_TRUE(TimedOutResult.Data->GetBoolField(TEXT("timedOut")));
	UNTEST_ASSERT_TRUE(TimedOutResult.Data->GetStringField(TEXT("diagnostics")).Contains(TEXT("singleton-roundtrip")));
	UNTEST_ASSERT_TRUE(TimedOutResult.Summary.Contains(TEXT("timedOut: true")));

	// Terminal state was consumed: the next poll is the exact unknown-id error.
	const IClaireonTool::FToolResult ConsumedResult =
		ClaireonWaitSupport::BuildWaitPollResult(Registry, WaitId);
	UNTEST_ASSERT_TRUE(ConsumedResult.bIsError);
	UNTEST_ASSERT_TRUE(ConsumedResult.ErrorMessage.Contains(TEXT("Unknown wait_id")));

	co_return;
}

#endif // WITH_UNTESTED
