// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// RetryRegister state-machine spec for FClaireonProxyClient (D4).
//
// Drives the four scenarios called out in
// without spawning a real proxy. The client exposes
// SetTransportOverrides_TestOnly + TickForTest seams so the spec can
// inject deterministic outcomes for /editor/register and /editor/heartbeat
// and step the state machine pulse-by-pulse.
//
// Category: Claireon.RetryRegister.* (run via
// `Scripts\Testing\Invoke-UntestTests.ps1 -TestFilter "Claireon.RetryRegister."`).

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonProxyClient.h"

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

namespace
{
	// File-local prefix on every helper to avoid colliding with other anon-NS
	// helpers under unity batching.

	/** Synthesise an FHeartbeatResult Ok=true (heartbeat 200 OK). */
	FHeartbeatResult RetryRegisterSpec_HeartbeatOk()
	{
		FHeartbeatResult R;
		R.Ok = true;
		R.Reason = EHeartbeatReason::None;
		return R;
	}

	/** Synthesise an FHeartbeatResult unknown_session WITHOUT evicted_by. */
	FHeartbeatResult RetryRegisterSpec_HeartbeatStale()
	{
		FHeartbeatResult R;
		R.Ok = false;
		R.Reason = EHeartbeatReason::UnknownSessionStale;
		return R;
	}

	/** Synthesise an FHeartbeatResult unknown_session WITH evicted_by. */
	FHeartbeatResult RetryRegisterSpec_HeartbeatEvicted(uint32 Pid, int64 StartTimeNs)
	{
		FHeartbeatResult R;
		R.Ok = false;
		R.Reason = EHeartbeatReason::UnknownSessionEvicted;
		R.EvictedByPid = Pid;
		R.EvictedByStartTimeNs = StartTimeNs;
		return R;
	}

	/**
	 * Seed a client with the cached registration parameters and start it in
	 * the RetryRegister state. Mirrors what BeginRetryRegister does at module
	 * StartServer time, minus the heartbeat ticker (we drive ticks by hand
	 * via TickForTest).
	 */
	void RetryRegisterSpec_SeedRetryRegister(FClaireonProxyClient& Client)
	{
		// 64-char dummy token (>=32 char minimum). Tests do not exercise the
		// token-length guard here; a separate ProxyClient smoke test covers
		// rejected tokens.
		Client.SeedCachedRegistration_TestOnly(
			/*EditorMCPPort=*/ 8017,
			/*EditorMCPToken=*/ TEXT("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"),
			/*BuildId=*/ TEXT("retry-register-spec"));
		Client.SetState_TestOnly(EClaireonProxyState::RetryRegister);
	}
} // namespace

// ---------------------------------------------------------------------------
// Scenario 1: cold start, transport returns Transient twice then Accepted.
// State walks RetryRegister -> RetryRegister -> Registered with backoff
// doubling on each Transient outcome.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, RetryRegister, ColdStartTransientThenAccepted, UNTEST_TIMEOUTMS(5000))
{
	FClaireonProxyClient Client;
	RetryRegisterSpec_SeedRetryRegister(Client);

	int32 RegisterCallCount = 0;
	Client.SetTransportOverrides_TestOnly(
		[&RegisterCallCount]() -> ERegisterResult
		{
			++RegisterCallCount;
			// First two attempts transient; third accepts.
			return RegisterCallCount < 3
				? ERegisterResult::Transient
				: ERegisterResult::Accepted;
		},
		TFunction<FHeartbeatResult()>());

	// Backoff floor before first tick.
	UNTEST_ASSERT_NEAR(Client.GetCurrentBackoffSeconds_TestOnly(), 0.25, 1e-6);

	// Tick 1 at NowSeconds=100. RegisterOnce override returns Transient ->
	// ScheduleRetry; backoff doubles to 0.5; NextRegisterAttempt = 100 + 0.25.
	Client.TickForTest(/*NowSeconds=*/ 100.0);
	UNTEST_ASSERT_EQ(static_cast<int32>(Client.GetState()), static_cast<int32>(EClaireonProxyState::RetryRegister));
	UNTEST_ASSERT_EQ(RegisterCallCount, 1);
	UNTEST_ASSERT_NEAR(Client.GetCurrentBackoffSeconds_TestOnly(), 0.5, 1e-6);
	UNTEST_ASSERT_NEAR(Client.GetNextRegisterAttemptSeconds_TestOnly(), 100.25, 1e-6);

	// Tick 2 at NowSeconds=101 (>= NextRegisterAttempt). Transient again ->
	// backoff doubles to 1.0; NextRegisterAttempt = 101 + 0.5.
	Client.TickForTest(/*NowSeconds=*/ 101.0);
	UNTEST_ASSERT_EQ(static_cast<int32>(Client.GetState()), static_cast<int32>(EClaireonProxyState::RetryRegister));
	UNTEST_ASSERT_EQ(RegisterCallCount, 2);
	UNTEST_ASSERT_NEAR(Client.GetCurrentBackoffSeconds_TestOnly(), 1.0, 1e-6);
	UNTEST_ASSERT_NEAR(Client.GetNextRegisterAttemptSeconds_TestOnly(), 101.5, 1e-6);

	// Tick 3 at NowSeconds=102 (>= NextRegisterAttempt). Accepted ->
	// state Registered; backoff resets to 0.25.
	Client.TickForTest(/*NowSeconds=*/ 102.0);
	UNTEST_ASSERT_EQ(static_cast<int32>(Client.GetState()), static_cast<int32>(EClaireonProxyState::Registered));
	UNTEST_ASSERT_EQ(RegisterCallCount, 3);
	UNTEST_ASSERT_NEAR(Client.GetCurrentBackoffSeconds_TestOnly(), 0.25, 1e-6);

	co_return;
}

// ---------------------------------------------------------------------------
// Scenario 2: heartbeat unknown_session WITHOUT evicted_by bounces back to
// RetryRegister with backoff reset to the 0.25s floor.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, RetryRegister, HeartbeatStaleBouncesToRetryRegister, UNTEST_TIMEOUTMS(5000))
{
	FClaireonProxyClient Client;
	RetryRegisterSpec_SeedRetryRegister(Client);

	// First, drive Accepted to enter Registered.
	Client.SetTransportOverrides_TestOnly(
		[]() -> ERegisterResult { return ERegisterResult::Accepted; },
		TFunction<FHeartbeatResult()>());
	Client.TickForTest(100.0);
	UNTEST_ASSERT_EQ(static_cast<int32>(Client.GetState()), static_cast<int32>(EClaireonProxyState::Registered));

	// Now swap heartbeat to return UnknownSessionStale; tick.
	Client.SetTransportOverrides_TestOnly(
		[]() -> ERegisterResult { return ERegisterResult::Accepted; },
		[]() -> FHeartbeatResult { return RetryRegisterSpec_HeartbeatStale(); });

	Client.TickForTest(110.0);
	UNTEST_ASSERT_EQ(static_cast<int32>(Client.GetState()), static_cast<int32>(EClaireonProxyState::RetryRegister));
	// Backoff resets to 0.25 floor (then doubled to 0.5 by ScheduleRetry).
	UNTEST_ASSERT_NEAR(Client.GetCurrentBackoffSeconds_TestOnly(), 0.5, 1e-6);
	UNTEST_ASSERT_NEAR(Client.GetNextRegisterAttemptSeconds_TestOnly(), 110.25, 1e-6);

	// Next tick (at 110.5, past NextRegisterAttempt) re-attempts register.
	int32 RegisterCallCount = 0;
	Client.SetTransportOverrides_TestOnly(
		[&RegisterCallCount]() -> ERegisterResult
		{
			++RegisterCallCount;
			return ERegisterResult::Accepted;
		},
		TFunction<FHeartbeatResult()>());
	Client.TickForTest(110.5);
	UNTEST_ASSERT_EQ(RegisterCallCount, 1);
	UNTEST_ASSERT_EQ(static_cast<int32>(Client.GetState()), static_cast<int32>(EClaireonProxyState::Registered));

	co_return;
}

// ---------------------------------------------------------------------------
// Scenario 3: heartbeat unknown_session WITH evicted_by transitions to
// Failed terminally; subsequent ticks are no-ops (no further register
// attempts are issued).
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, RetryRegister, HeartbeatEvictedTransitionsToFailed, UNTEST_TIMEOUTMS(5000))
{
	FClaireonProxyClient Client;
	RetryRegisterSpec_SeedRetryRegister(Client);

	// Drive Accepted to enter Registered.
	int32 RegisterCallCount = 0;
	Client.SetTransportOverrides_TestOnly(
		[&RegisterCallCount]() -> ERegisterResult
		{
			++RegisterCallCount;
			return ERegisterResult::Accepted;
		},
		TFunction<FHeartbeatResult()>());
	Client.TickForTest(100.0);
	UNTEST_ASSERT_EQ(RegisterCallCount, 1);

	// Now return evicted heartbeat.
	Client.SetTransportOverrides_TestOnly(
		[&RegisterCallCount]() -> ERegisterResult
		{
			++RegisterCallCount;
			return ERegisterResult::Accepted;
		},
		[]() -> FHeartbeatResult { return RetryRegisterSpec_HeartbeatEvicted(/*Pid=*/ 9999, /*StartTimeNs=*/ 12345); });

	Client.TickForTest(110.0);
	UNTEST_ASSERT_EQ(static_cast<int32>(Client.GetState()), static_cast<int32>(EClaireonProxyState::Failed));

	// Subsequent ticks are no-ops.
	const int32 RegisterCallsAtFailed = RegisterCallCount;
	Client.TickForTest(111.0);
	Client.TickForTest(112.0);
	UNTEST_ASSERT_EQ(RegisterCallCount, RegisterCallsAtFailed);
	UNTEST_ASSERT_EQ(static_cast<int32>(Client.GetState()), static_cast<int32>(EClaireonProxyState::Failed));

	co_return;
}

// ---------------------------------------------------------------------------
// Scenario 4: RequestReconnect transitions Failed -> RetryRegister; the
// next tick with Accepted lands in Registered.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, RetryRegister, ReconnectFromFailed, UNTEST_TIMEOUTMS(5000))
{
	FClaireonProxyClient Client;
	RetryRegisterSpec_SeedRetryRegister(Client);

	// Land in Registered, then evict to Failed.
	Client.SetTransportOverrides_TestOnly(
		[]() -> ERegisterResult { return ERegisterResult::Accepted; },
		[]() -> FHeartbeatResult { return RetryRegisterSpec_HeartbeatEvicted(7777, 99999); });
	Client.TickForTest(100.0); // RegisterOnce override -> Accepted -> Registered.
	Client.TickForTest(105.0); // Heartbeat override -> Evicted -> Failed.
	UNTEST_ASSERT_EQ(static_cast<int32>(Client.GetState()), static_cast<int32>(EClaireonProxyState::Failed));

	// Reconnect: Failed -> RetryRegister, NextRegisterAttempt cleared.
	Client.RequestReconnect();
	UNTEST_ASSERT_EQ(static_cast<int32>(Client.GetState()), static_cast<int32>(EClaireonProxyState::RetryRegister));

	// Tick with Accepted; Failed -> RetryRegister -> Registered.
	int32 RegisterCallCount = 0;
	Client.SetTransportOverrides_TestOnly(
		[&RegisterCallCount]() -> ERegisterResult
		{
			++RegisterCallCount;
			return ERegisterResult::Accepted;
		},
		// Keep heartbeat override returning Ok so the real-time heartbeat
		// ticker (started by RequestReconnect) cannot bounce us back to
		// RetryRegister mid-test if it fires before TickForTest below.
		[]() -> FHeartbeatResult { return RetryRegisterSpec_HeartbeatOk(); });

	// RequestReconnect set NextRegisterAttemptSeconds = FPlatformTime::Seconds().
	// We need to tick at >= that. Use a large fake NowSeconds to ensure it's past.
	Client.TickForTest(1.0e10);
	UNTEST_ASSERT_GE(RegisterCallCount, 1);
	UNTEST_ASSERT_EQ(static_cast<int32>(Client.GetState()), static_cast<int32>(EClaireonProxyState::Registered));

	co_return;
}

#endif // WITH_UNTESTED
