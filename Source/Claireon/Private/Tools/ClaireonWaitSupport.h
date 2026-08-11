// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Templates/Function.h"
#include "Tools/IClaireonTool.h"

/**
 * WI-8 shared support for the non-blocking wait tools (pie_wait_for /
 * pie_wait_poll) and the test.run deadline policy.
 *
 * The old pie_wait_for implementation sleep-polled on the game thread, which
 * starved the very ticks its conditions (mapLoad, pieReady, actorValid,
 * initState) needed to become true: a condition that was not already true
 * could NEVER become true, and the tool burned the full timeout frozen. The
 * registry below replaces that with a frame-ticked wait record plus an
 * explicit poll: nothing blocks, the editor keeps ticking, and the caller
 * retrieves the terminal state with a follow-up call.
 *
 * Game-thread only: StartWait/TickWaits/Poll are all expected to run on the
 * game thread (tool Execute and FTSTicker both do), so no locking is used.
 */
class FClaireonPIEWaitRegistry
{
public:
	enum class EWaitState : uint8
	{
		Pending,
		Met,
		TimedOut
	};

	/** Snapshot of one wait's state as reported by Poll(). */
	struct FWaitStatus
	{
		/** False when the wait id is unknown (never started, or terminal state already consumed). */
		bool bFound = false;
		EWaitState State = EWaitState::Pending;
		FString ConditionName;
		double ElapsedSeconds = 0.0;
		double TimeoutSeconds = 0.0;
		/** Populated (via the wait's diagnostics provider) only when State == TimedOut. */
		FString TimeoutDiagnostics;
	};

	/**
	 * @param InClock          Seconds source. Defaults to FPlatformTime::Seconds.
	 *                         Tests inject a fake clock so deadline behavior is
	 *                         verified without sleeping the test thread.
	 * @param bInUseCoreTicker When true (the production singleton), StartWait
	 *                         registers an FTSTicker delegate that re-checks
	 *                         every pending wait each frame. Test instances
	 *                         pass false and drive TickWaits()/Poll() directly.
	 */
	explicit FClaireonPIEWaitRegistry(TFunction<double()> InClock = TFunction<double()>(), bool bInUseCoreTicker = false);
	~FClaireonPIEWaitRegistry();

	FClaireonPIEWaitRegistry(const FClaireonPIEWaitRegistry&) = delete;
	FClaireonPIEWaitRegistry& operator=(const FClaireonPIEWaitRegistry&) = delete;

	/**
	 * Register a wait. The condition is re-evaluated once per frame (core
	 * ticker) and once per Poll() until it returns true or the deadline
	 * passes. Returns the new wait id.
	 *
	 * @param TimeoutDiagnosticsProvider Optional callback invoked exactly once
	 *        if and when the wait times out; its return value is surfaced in
	 *        FWaitStatus::TimeoutDiagnostics. Invoked at timeout-detection
	 *        time so it reports live state.
	 */
	FString StartWait(
		const FString& ConditionName,
		TFunction<bool()> Condition,
		double TimeoutSeconds,
		TFunction<FString()> TimeoutDiagnosticsProvider = TFunction<FString()>());

	/**
	 * Advance every pending wait once: evaluate its condition, then its
	 * deadline. Called by the core ticker each frame in production; tests
	 * call it directly after moving the injected clock.
	 */
	void TickWaits();

	/**
	 * Report the wait's current state. Pending waits are advanced first (so a
	 * poll observes a deadline even if no tick ran in between). A terminal
	 * (Met/TimedOut) state is CONSUMED: the record is removed and a second
	 * poll of the same id returns bFound == false.
	 */
	FWaitStatus Poll(const FString& WaitId);

	/** Number of waits still pending (not yet terminal). */
	int32 NumPendingWaits() const;

	/** Number of tracked records, pending or terminal-but-unconsumed. */
	int32 NumTrackedWaits() const;

	/** Production singleton: real clock, core-ticker-driven. */
	static FClaireonPIEWaitRegistry& Get();

private:
	struct FWaitRecord
	{
		FString ConditionName;
		TFunction<bool()> Condition;
		TFunction<FString()> TimeoutDiagnosticsProvider;
		double StartSeconds = 0.0;
		double TimeoutSeconds = 0.0;
		EWaitState State = EWaitState::Pending;
		/** Elapsed at the moment the wait went terminal. */
		double TerminalElapsedSeconds = 0.0;
		/** Clock time the wait went terminal (drives retention pruning). */
		double TerminalAtSeconds = 0.0;
		FString TimeoutDiagnostics;
	};

	double ReadClockSeconds() const;
	void AdvanceRecord(FWaitRecord& Record, double InNowSeconds);
	void PruneExpiredTerminalRecords(double InNowSeconds);
	void EnsureCoreTicker();

	TMap<FString, FWaitRecord> Waits;
	TFunction<double()> Clock;
	bool bUseCoreTicker = false;
	FTSTicker::FDelegateHandle CoreTickerHandle;
};

namespace ClaireonWaitSupport
{
	/** Hard ceiling applied to test.run when noTimeout=true. The run is never unbounded. */
	inline constexpr double TestRunNoTimeoutCapSeconds = 3600.0;

	/** Default test.run completion timeout when noTimeout is false/absent. */
	inline constexpr double TestRunDefaultTimeoutSeconds = 1800.0;

	/** Terminal-but-never-polled wait records are pruned after this long. */
	inline constexpr double TerminalWaitRetentionSeconds = 900.0;

	/**
	 * Resolve the effective test.run completion deadline. noTimeout=true
	 * extends the deadline to TestRunNoTimeoutCapSeconds -- it never resolves
	 * to numeric max (the old behavior was an unbounded game-thread block
	 * with no cancellation).
	 */
	double ResolveTestRunTimeoutSeconds(bool bNoTimeout);

	/** Outcome of StartOrCompleteWait. */
	struct FStartWaitOutcome
	{
		/** True when the condition was already true: nothing was registered. */
		bool bImmediatelyMet = false;
		/** Wait id to poll; set only when a wait was registered. */
		FString WaitId;
	};

	/**
	 * Evaluate Condition once; if already true, return immediately with no
	 * wait registered. Otherwise register a frame-ticked wait in Registry and
	 * return its id.
	 */
	FStartWaitOutcome StartOrCompleteWait(
		FClaireonPIEWaitRegistry& Registry,
		const FString& ConditionName,
		TFunction<bool()> Condition,
		double TimeoutSeconds,
		TFunction<FString()> TimeoutDiagnosticsProvider);

	/**
	 * Poll a wait id and translate the outcome into a tool result. Shared by
	 * pie_wait_for (wait_id argument) and pie_wait_poll.
	 *
	 * - unknown id            -> error naming the id.
	 * - pending               -> success, data.status == "waiting".
	 * - met                   -> success, conditionMet=true, timedOut=false.
	 * - timed out (consumed)  -> success, conditionMet=false, timedOut=true,
	 *                            with the wait's timeout diagnostics attached.
	 */
	IClaireonTool::FToolResult BuildWaitPollResult(FClaireonPIEWaitRegistry& Registry, const FString& WaitId);
}
