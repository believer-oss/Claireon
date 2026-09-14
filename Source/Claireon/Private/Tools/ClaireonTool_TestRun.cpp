// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TestRun.h"
#include "ClaireonLog.h"
#include "ClaireonSessionManager.h"
#include "Tools/ClaireonTestRunStrictEval.h"
#include "Tools/ClaireonWaitSupport.h"

#include "Async/TaskGraphInterfaces.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IAutomationControllerModule.h"
#include "IAutomationReport.h"
#include "IAutomationWorkerModule.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/FilterCollection.h"
#include "Misc/Paths.h"

namespace ClaireonToolTestRunInternal
{
	/**
	 * Let engine frames advance automation between calls; do not pump inside Execute.
	 * MCP dispatch already runs inside the core ticker, which cannot be ticked recursively.
	 */

	/** Pre-run phases advance one step per poll, with engine frames doing the work between calls. */
	enum class ETestRunPhase : uint8
	{
		AwaitWorkers,
		AwaitTests,
		Running,
	};

	const TCHAR* TestRun_PhaseName(ETestRunPhase Phase)
	{
		switch (Phase)
		{
		case ETestRunPhase::AwaitWorkers: return TEXT("awaiting_workers");
		case ETestRunPhase::AwaitTests:   return TEXT("awaiting_test_list");
		case ETestRunPhase::Running:      return TEXT("running");
		}
		return TEXT("unknown");
	}

	/**
	 * One in-memory run at a time: concurrent runs would share the automation controller.
	 * An editor restart discards the run.
	 */
	struct FTestRunState
	{
		bool bActive = false;
		ETestRunPhase Phase = ETestRunPhase::AwaitWorkers;
		TArray<FString> MatchingTestNames;
		double PhaseStartSeconds = 0.0;
		FString RunId;
		FString TestFilter;
		ClaireonTestRunStrict::FSpec Strict;
		TSet<FString> DiscoveredTestNames;
		double StartSeconds = 0.0;
		double TimeoutSeconds = 0.0;
		int32 TotalEnabled = 0;
	};

	FTestRunState& TestRun_State()
	{
		static FTestRunState State;
		return State;
	}

	bool TestRun_IsActive(FString& OutRunId)
	{
		const FTestRunState& State = TestRun_State();
		OutRunId = State.RunId;
		return State.bActive;
	}

	constexpr double TESTRUN_MIN_POLL_SECONDS = 5.0;

	/** Below this threshold, suggest the full remaining wait instead of halving it. */
	constexpr double TESTRUN_TAIL_SECONDS = 20.0;

	/** Seed pace until the first test completes. */
	constexpr double TESTRUN_SEED_SECONDS_PER_TEST = 2.0;

	/** Suggest a poll delay using this run's observed pace when available. */
	double TestRun_SuggestPollDelaySeconds(int32 Completed, int32 Total, double ElapsedSeconds)
	{
		const int32 Remaining = FMath::Max(0, Total - Completed);
		if (Remaining == 0)
		{
			return TESTRUN_MIN_POLL_SECONDS;
		}

		const double PerTest = (Completed > 0 && ElapsedSeconds > 0.0)
			? (ElapsedSeconds / static_cast<double>(Completed))
			: TESTRUN_SEED_SECONDS_PER_TEST;

		const double FullRemaining = static_cast<double>(Remaining) * PerTest;
		const double Suggested = (FullRemaining <= TESTRUN_TAIL_SECONDS)
			? FullRemaining
			: 0.5 * FullRemaining;

		return FMath::Max(TESTRUN_MIN_POLL_SECONDS, Suggested);
	}

	/** Count terminal states among enabled reports. */
	int32 TestRun_CountCompleted(IAutomationControllerManagerRef Controller, int32& OutTotal)
	{
		const TArray<TSharedPtr<IAutomationReport>> Reports = Controller->GetEnabledReports();
		OutTotal = Reports.Num();
		if (Controller->GetNumDeviceClusters() == 0)
		{
			return 0;
		}

		int32 Completed = 0;
		for (const TSharedPtr<IAutomationReport>& Report : Reports)
		{
			if (!Report.IsValid())
			{
				continue;
			}
			switch (Report->GetState(0, 0))
			{
			case EAutomationState::Success:
			case EAutomationState::Fail:
			case EAutomationState::Skipped:
				++Completed;
				break;
			default:
				break;
			}
		}
		return Completed;
	}

	/** Reset run state after reporting completion. */
	void TestRun_Release(FTestRunState& State)
	{
		State = FTestRunState();
	}
} // namespace ClaireonToolTestRunInternal

using namespace ClaireonToolTestRunInternal;

bool ClaireonTestRun_IsRunActive(FString& OutRunId)
{
	return ClaireonToolTestRunInternal::TestRun_IsActive(OutRunId);
}

FString ClaireonTool_TestRun::GetCategory() const { return TEXT("test"); }
FString ClaireonTool_TestRun::GetOperation() const { return TEXT("run"); }

FString ClaireonTool_TestRun::GetDescription() const
{
    return TEXT("Start an automation run and return at once with a run_id; poll with test_poll. Does "
                "NOT block: the run advances on the editor's own frames. While it is active the "
                "bridge refuses every tool except test_run, test_poll, tool_search and session "
                "recovery, and that gate has NO autonomous expiry: only a test_poll that observes "
                "completion, the deadline, or cancel=true releases it.");
}

TSharedPtr<FJsonObject> ClaireonTool_TestRun::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> FilterProp = MakeShared<FJsonObject>();
	FilterProp->SetStringField(TEXT("type"), TEXT("string"));
	FilterProp->SetStringField(TEXT("description"),
		TEXT("Filter pattern to select specific tests (e.g. 'Combat', 'Ability.Fire')"));
	Properties->SetObjectField(TEXT("testFilter"), FilterProp);

	TSharedPtr<FJsonObject> NoTimeoutProp = MakeShared<FJsonObject>();
	NoTimeoutProp->SetStringField(TEXT("type"), TEXT("boolean"));
	NoTimeoutProp->SetStringField(TEXT("description"),
		TEXT("Extend the run deadline to the hard cap of 3600 seconds instead of the default 1800 seconds (useful for debugging, default: false). The run is never unbounded: on reaching the deadline the tests are stopped and the run is reported as timed out."));
	Properties->SetObjectField(TEXT("noTimeout"), NoTimeoutProp);


	TSharedPtr<FJsonObject> StrictProp = MakeShared<FJsonObject>();
	StrictProp->SetStringField(TEXT("type"), TEXT("boolean"));
	StrictProp->SetStringField(TEXT("description"),
		TEXT("Treat a run that executed nothing as a failure (default: false, preserving the permissive default for existing callers). With strict=true the run errors unless at least one test was enabled and every one of them reached a terminal state: zero failed, zero not-run, zero still-in-progress, no timeout. Supplying expected_tests or allowed_skips implies strict=true."));
	Properties->SetObjectField(TEXT("strict"), StrictProp);

	TSharedPtr<FJsonObject> ExpectedProp = MakeShared<FJsonObject>();
	ExpectedProp->SetStringField(TEXT("type"), TEXT("array"));
	{
		TSharedPtr<FJsonObject> ItemsProp = MakeShared<FJsonObject>();
		ItemsProp->SetStringField(TEXT("type"), TEXT("string"));
		ExpectedProp->SetObjectField(TEXT("items"), ItemsProp);
	}
	ExpectedProp->SetStringField(TEXT("description"),
		TEXT("The full test-path set this run must discover, e.g. ['Claireon.BPEditor.Substrate.TransactionBufferIsReal']. Compared for set equality against the discovered set BEFORE anything runs and before any skip allowlist is consulted, so a test that disappeared from the build fails the run instead of vanishing. A count is never the proof: a renamed test can replace a missing one while the count holds."));
	Properties->SetObjectField(TEXT("expected_tests"), ExpectedProp);

	TSharedPtr<FJsonObject> SkipsProp = MakeShared<FJsonObject>();
	SkipsProp->SetStringField(TEXT("type"), TEXT("array"));
	{
		TSharedPtr<FJsonObject> ItemsProp = MakeShared<FJsonObject>();
		ItemsProp->SetStringField(TEXT("type"), TEXT("object"));
		SkipsProp->SetObjectField(TEXT("items"), ItemsProp);
	}
	SkipsProp->SetStringField(TEXT("description"),
		TEXT("Individually named skips that are permitted, as [{'name': '<full test path>', 'reason': '<why>'}]. Governs EXECUTION OUTCOMES only -- allowlisted names are never removed from either side of the expected_tests comparison, because doing so would let an allowlisted-but-undiscovered test pass vacuously. Any observed skip not on this list fails the run, and an entry without a reason is rejected."));
	Properties->SetObjectField(TEXT("allowed_skips"), SkipsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_TestRun::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString TestFilter;
	bool bNoTimeout = false;

	if (Arguments.IsValid())
	{
		Arguments->TryGetStringField(TEXT("testFilter"), TestFilter);
		Arguments->TryGetBoolField(TEXT("noTimeout"), bNoTimeout);
	}

	ClaireonTestRunStrict::FSpec Strict;
	{
		FString StrictParseError;
		if (!ClaireonTestRunStrict::ParseSpec(Arguments, Strict, StrictParseError))
		{
			return MakeErrorResult(StrictParseError);
		}
	}

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] editor.test.run: testFilter='%s', noTimeout=%s, strict=%s (expected_tests=%d, allowed_skips=%d)"),
		*TestFilter, bNoTimeout ? TEXT("true") : TEXT("false"),
		Strict.bEnabled ? TEXT("true") : TEXT("false"),
		Strict.ExpectedTests.Num(), Strict.AllowedSkips.Num());

	if (TestFilter.IsEmpty())
	{
		return MakeErrorResult(TEXT("testFilter is required to avoid running all tests accidentally."));
	}

	// Load modules and widen discovery beyond the default smoke filter.
	// The worker overwrites the global filter on each RequestTests.
	FAutomationTestFramework::Get().LoadTestModules();
	FAutomationTestFramework::Get().SetRequestedTestFilter(EAutomationTestFlags_FilterMask);

	TArray<FAutomationTestInfo> AllTestInfos;
	FAutomationTestFramework::Get().GetValidTestNames(AllTestInfos);

	TArray<FString> MatchingTestNames;
	for (const FAutomationTestInfo& TestInfo : AllTestInfos)
	{
		const FString& DisplayName = TestInfo.GetDisplayName();
		const FString& FullPath = TestInfo.GetFullTestPath();

		if (DisplayName.Contains(TestFilter) || FullPath.Contains(TestFilter))
		{
			MatchingTestNames.Add(FullPath);
		}
	}

	if (MatchingTestNames.Num() == 0)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("No automation tests found matching filter '%s'. Use editor.test.list to see available tests."),
			*TestFilter));
	}

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] Found %d tests matching filter '%s'"), MatchingTestNames.Num(), *TestFilter);

	// Validate discovery before applying allowed skips; a missing test must not pass through allowlisting.
	const TSet<FString> DiscoveredTestNames(MatchingTestNames);
	{
		const ClaireonTestRunStrict::FVerdict Discovery =
			ClaireonTestRunStrict::EvaluateDiscovery(Strict, TestFilter, DiscoveredTestNames);
		if (Discovery.bFailed)
		{
			FToolResult DiscoveryResult = MakeErrorResult(Discovery.Message);
			DiscoveryResult.Data = Discovery.Data;
			return DiscoveryResult;
		}
	}

	// test_poll enforces this deadline. An abandoned run keeps the bridge gate active until polled.
	const double EffectiveRunTimeoutSeconds = ClaireonWaitSupport::ResolveTestRunTimeoutSeconds(bNoTimeout);

	IAutomationControllerModule& AutomationModule =
		FModuleManager::LoadModuleChecked<IAutomationControllerModule>(TEXT("AutomationController"));
	IAutomationControllerManagerRef Controller = AutomationModule.GetAutomationController();

	// Load the worker so engine frames can process its message inbox.
	IAutomationWorkerModule* WorkerModule =
		FModuleManager::Get().LoadModulePtr<IAutomationWorkerModule>(TEXT("AutomationWorker"));
	if (WorkerModule == nullptr)
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[MCP] AutomationWorker module is unavailable. Its inbox is the only delivery path for the ")
			TEXT("controller's worker messages while the engine loop is parked, so this run will stall."));
	}

	Controller->Init();

	Controller->RequestAvailableWorkers(FApp::GetSessionId());

	// Return so engine frames can process worker discovery and test reports.
	FTestRunState& State = TestRun_State();
	State = FTestRunState();
	State.bActive = true;
	State.Phase = ETestRunPhase::AwaitWorkers;
	State.RunId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens);
	State.TestFilter = TestFilter;
	State.Strict = Strict;
	State.DiscoveredTestNames = DiscoveredTestNames;
	State.MatchingTestNames = MatchingTestNames;
	State.StartSeconds = FPlatformTime::Seconds();
	State.PhaseStartSeconds = State.StartSeconds;
	State.TimeoutSeconds = EffectiveRunTimeoutSeconds;
	State.TotalEnabled = MatchingTestNames.Num();

	TSharedPtr<FJsonObject> StartData = MakeShared<FJsonObject>();
	StartData->SetStringField(TEXT("run_id"), State.RunId);
	StartData->SetStringField(TEXT("state"), TestRun_PhaseName(State.Phase));
	StartData->SetNumberField(TEXT("total"), State.TotalEnabled);
	StartData->SetNumberField(TEXT("completed"), 0);
	StartData->SetNumberField(TEXT("suggested_poll_after_seconds"), TESTRUN_MIN_POLL_SECONDS);
	StartData->SetStringField(TEXT("poll_with"), TEXT("test_poll"));

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] test_run started run %s: %d test(s) matched, filter '%s', timeout %.0fs"),
		*State.RunId, State.TotalEnabled, *State.TestFilter, State.TimeoutSeconds);

	return MakeSuccessResult(StartData, FString::Printf(
		TEXT("Started run %s: %d test(s) matched for filter '%s'. This call did NOT block. ")
		TEXT("The automation controller now digests the worker's test list on the editor's own ")
		TEXT("frames -- on this project that is a tree of 300k+ tests and takes SEVERAL MINUTES ")
		TEXT("before the first test runs, which is expected and is not a hang. Call ")
		TEXT("test_poll(run_id='%s') and obey suggested_poll_after_seconds each time. Other MCP ")
		TEXT("tool calls are refused until the run finishes or is cancelled."),
		*State.RunId, State.TotalEnabled, *State.TestFilter, *State.RunId));
}



FString ClaireonTool_TestPoll::GetCategory() const { return TEXT("test"); }
FString ClaireonTool_TestPoll::GetOperation() const { return TEXT("poll"); }

FString ClaireonTool_TestPoll::GetDescription() const
{
    return TEXT("Poll the automation run started by test_run, or cancel it. Returns progress while "
                "it runs and the full report -- including the strict verdict -- once it finishes. "
                "Every reply carries suggested_poll_after_seconds so a caller waits instead of "
                "spinning. Non-session itself; releases the run's editor-wide session on completion.");
}

TSharedPtr<FJsonObject> ClaireonTool_TestPoll::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> RunIdProp = MakeShared<FJsonObject>();
	RunIdProp->SetStringField(TEXT("type"), TEXT("string"));
	RunIdProp->SetStringField(TEXT("description"),
		TEXT("The run_id test_run returned. Optional: with no run_id the active run is polled. Supplying it is safer -- a mismatch is reported rather than silently answering about a different run."));
	Properties->SetObjectField(TEXT("run_id"), RunIdProp);

	TSharedPtr<FJsonObject> CancelProp = MakeShared<FJsonObject>();
	CancelProp->SetStringField(TEXT("type"), TEXT("boolean"));
	CancelProp->SetStringField(TEXT("description"),
		TEXT("Stop the run instead of waiting for it (default: false). Tests already finished are still reported; the editor-wide session is released either way."));
	Properties->SetObjectField(TEXT("cancel"), CancelProp);

	Schema->SetObjectField(TEXT("properties"), Properties);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_TestPoll::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString RequestedRunId;
	bool bCancel = false;
	if (Arguments.IsValid())
	{
		Arguments->TryGetStringField(TEXT("run_id"), RequestedRunId);
		Arguments->TryGetBoolField(TEXT("cancel"), bCancel);
	}

	FTestRunState& State = TestRun_State();
	if (!State.bActive)
	{
		return MakeErrorResult(TEXT(
			"No test run is active. Either none was started, one already completed and was "
			"reported (a finished run is reported exactly once, then forgotten), or the editor "
			"restarted. Start one with test_run."));
	}
	if (!RequestedRunId.IsEmpty() && RequestedRunId != State.RunId)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("run_id '%s' does not match the active run '%s'. Answering about a different run "
			     "than the one asked about would be worse than refusing."),
			*RequestedRunId, *State.RunId));
	}

	IAutomationControllerModule& AutomationModule =
		FModuleManager::LoadModuleChecked<IAutomationControllerModule>(TEXT("AutomationController"));
	IAutomationControllerManagerRef Controller = AutomationModule.GetAutomationController();

	const FString& TestFilter = State.TestFilter;
	const ClaireonTestRunStrict::FSpec& Strict = State.Strict;
	const TSet<FString>& DiscoveredTestNames = State.DiscoveredTestNames;
	const double EffectiveTimeout = State.TimeoutSeconds;
	const double TotalDuration = FPlatformTime::Seconds() - State.StartSeconds;

	// Advance one pre-run phase per poll.
	if (!bCancel && State.Phase != ETestRunPhase::Running)
	{
		const double PhaseSeconds = FPlatformTime::Seconds() - State.PhaseStartSeconds;

		if (State.Phase == ETestRunPhase::AwaitWorkers)
		{
			if (Controller->GetNumDeviceClusters() > 0)
			{
				Controller->RequestTests();
				State.Phase = ETestRunPhase::AwaitTests;
				State.PhaseStartSeconds = FPlatformTime::Seconds();
			}
		}
		else if (State.Phase == ETestRunPhase::AwaitTests)
		{
			// Check readiness by enabling the requested tests and reading the enabled count.
			Controller->SetVisibleTestsEnabled(false);
			Controller->SetEnabledTests(State.MatchingTestNames);
			const int32 EnabledCount = Controller->GetEnabledTestsNum();

			if (EnabledCount > 0)
			{
				State.TotalEnabled = EnabledCount;
				Controller->SetNumPasses(1);
				Controller->RunTests(/* bIsLocalSession */ true);
				State.Phase = ETestRunPhase::Running;
				State.PhaseStartSeconds = FPlatformTime::Seconds();

				UE_LOG(LogClaireon, Display,
					TEXT("[MCP] Run %s: enabled %d test(s) and started execution after %.0fs"),
					*State.RunId, EnabledCount, TotalDuration);
			}
		}

		if (State.Phase != ETestRunPhase::Running)
		{
			if (TotalDuration > EffectiveTimeout)
			{
				TestRun_Release(State);
				return MakeErrorResult(FString::Printf(
					TEXT("Run gave up after %.0fs still in phase '%s'. The automation controller "
					     "never became ready -- on a project with a very large test tree this "
					     "phase legitimately takes minutes, so a timeout here means something is "
					     "wrong rather than slow."),
					TotalDuration, TestRun_PhaseName(State.Phase)));
			}

			TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
			Progress->SetStringField(TEXT("run_id"), State.RunId);
			Progress->SetStringField(TEXT("state"), TestRun_PhaseName(State.Phase));
			Progress->SetNumberField(TEXT("completed"), 0);
			Progress->SetNumberField(TEXT("total"), State.TotalEnabled);
			Progress->SetNumberField(TEXT("elapsed_seconds"), TotalDuration);
			Progress->SetNumberField(TEXT("suggested_poll_after_seconds"), 30.0);

			return MakeSuccessResult(Progress, FString::Printf(
				TEXT("Run %s is in phase '%s' after %.0fs (%.0fs in this phase). No test has "
				     "started yet: the controller is digesting the worker's test list, which on "
				     "this project takes SEVERAL MINUTES and is not a hang. WAIT ABOUT 30 "
				     "SECONDS, then call test_poll again."),
				*State.RunId, TestRun_PhaseName(State.Phase), TotalDuration, PhaseSeconds));
		}
	}

	const bool bStillRunning =
		Controller->GetTestState() == EAutomationControllerModuleState::Running;
	bool bTimedOut = false;

	if (bStillRunning && !bCancel)
	{
		// Only polling enforces the deadline and releases the bridge gate.
		if (TotalDuration > EffectiveTimeout)
		{
			Controller->StopTests();
			UE_LOG(LogClaireon, Warning,
				TEXT("[MCP] Test run %s timed out after %.0fs, stopping tests"),
				*State.RunId, EffectiveTimeout);
			bTimedOut = true;
		}
		else
		{
			int32 Total = State.TotalEnabled;
			const int32 Completed = TestRun_CountCompleted(Controller, Total);
			const double NextPoll = TestRun_SuggestPollDelaySeconds(Completed, Total, TotalDuration);

			TSharedPtr<FJsonObject> Progress = MakeShared<FJsonObject>();
			Progress->SetStringField(TEXT("run_id"), State.RunId);
			Progress->SetStringField(TEXT("state"), TEXT("running"));
			Progress->SetNumberField(TEXT("completed"), Completed);
			Progress->SetNumberField(TEXT("total"), Total);
			Progress->SetNumberField(TEXT("elapsed_seconds"), TotalDuration);
			Progress->SetNumberField(TEXT("suggested_poll_after_seconds"), NextPoll);

			return MakeSuccessResult(Progress, FString::Printf(
				TEXT("Run %s: %d/%d complete after %.0fs. WAIT ABOUT %.0f SECONDS, then call "
				     "test_poll again. Do not poll sooner -- nothing changes faster than the "
				     "tests finish, and each poll costs a round trip."),
				*State.RunId, Completed, Total, TotalDuration, NextPoll));
		}
	}
	else if (bCancel && bStillRunning)
	{
		Controller->StopTests();
		UE_LOG(LogClaireon, Display,
			TEXT("[MCP] Test run %s cancelled after %.0fs"), *State.RunId, TotalDuration);
	}

	// Finished, timed out, or cancelled: build the report once, release, and forget.
	const FToolResult Report = [&]() -> FToolResult
	{
		TArray<TSharedPtr<IAutomationReport>> EnabledReports = Controller->GetEnabledReports();

		int32 Passed = 0;
		int32 Failed = 0;
		int32 Skipped = 0;
		int32 NotRun = 0;
		int32 InProcess = 0;

		FString DetailedResults;

		// Compare test names as well as counts; equal counts can conceal missing or substituted tests.
		TSet<FString> ExecutedNames;
		TSet<FString> FailedNames;
		TSet<FString> SkippedNames;
		TSet<FString> NotRunNames;
		TSet<FString> InProcessNames;

		for (const TSharedPtr<IAutomationReport>& Report : EnabledReports)
		{
			if (!Report.IsValid())
			{
				continue;
			}

			const FString& TestDisplayName = Report->GetDisplayName();
			// GetEnabledReports returns leaf reports only.
			const FString TestFullPath = Report->GetFullTestPath();
			ExecutedNames.Add(TestFullPath);

			// Get results for the first cluster, first pass
			const int32 NumClusters = Controller->GetNumDeviceClusters();
			EAutomationState TestState = EAutomationState::NotRun;

			if (NumClusters > 0)
			{
				TestState = Report->GetState(0, 0);
			}

			FString StateStr;
			switch (TestState)
			{
				case EAutomationState::Success:
					Passed++;
					StateStr = TEXT("PASS");
					break;
				case EAutomationState::Fail:
					Failed++;
					FailedNames.Add(TestFullPath);
					StateStr = TEXT("FAIL");
					break;
				case EAutomationState::Skipped:
					Skipped++;
					SkippedNames.Add(TestFullPath);
					StateStr = TEXT("SKIP");
					break;
				case EAutomationState::InProcess:
					InProcess++;
					InProcessNames.Add(TestFullPath);
					StateStr = TEXT("IN_PROGRESS");
					break;
				case EAutomationState::NotRun:
				default:
					NotRun++;
					NotRunNames.Add(TestFullPath);
					StateStr = TEXT("NOT_RUN");
					break;
			}

			DetailedResults += FString::Printf(TEXT("  [%s] %s\n"), *StateStr, *TestDisplayName);

			if (TestState == EAutomationState::Fail && NumClusters > 0)
			{
				const FAutomationTestResults& Results = Report->GetResults(0, 0);
				const TArray<FAutomationExecutionEntry>& Entries = Results.GetEntries();
				for (const FAutomationExecutionEntry& Entry : Entries)
				{
					if (Entry.Event.Type == EAutomationEventType::Error)
					{
						DetailedResults += FString::Printf(TEXT("    ERROR: %s\n"), *Entry.Event.Message);
					}
				}
			}
		}

		FString Result;

		if (bTimedOut)
		{
			Result += FString::Printf(
				TEXT("Tests TIMED OUT after %.1f seconds (limit: %.0f seconds)\n"),
				TotalDuration, EffectiveTimeout);
		}
		else if (Failed == 0 && NotRun == 0 && InProcess == 0)
		{
			Result += TEXT("Tests completed successfully\n");
		}
		else if (Failed > 0)
		{
			Result += FString::Printf(TEXT("Tests FAILED (%d failures)\n"), Failed);
		}
		else
		{
			Result += TEXT("Tests completed with some tests not run\n");
		}

		Result += FString::Printf(TEXT("Duration: %.1f seconds\n"), TotalDuration);
		Result += FString::Printf(TEXT("Test Filter: %s\n"), *TestFilter);
		Result += FString::Printf(TEXT("Results: %d passed, %d failed, %d skipped, %d not run\n"),
			Passed, Failed, Skipped, NotRun);

		if (bTimedOut)
		{
			Result += TEXT("Timed Out: true\n");
		}

		if (InProcess > 0)
		{
			Result += FString::Printf(TEXT("Still In Progress: %d\n"), InProcess);
		}

		Result += TEXT("\n--- Test Results ---\n");
		Result += DetailedResults;

		UE_LOG(LogClaireon, Display,
			TEXT("[MCP] Tests completed: passed=%d, failed=%d, skipped=%d, notRun=%d, duration=%.1fs, timedOut=%s"),
			Passed, Failed, Skipped, NotRun, TotalDuration,
			bTimedOut ? TEXT("true") : TEXT("false"));

		if (Strict.bEnabled)
		{
			ClaireonTestRunStrict::FExecutionObservation Observation;
			Observation.ReportCount = EnabledReports.Num();
			Observation.bTimedOut = bTimedOut;
			Observation.TimeoutSeconds = EffectiveTimeout;
			Observation.Discovered = DiscoveredTestNames;
			Observation.Reported = ExecutedNames;
			Observation.Failed = FailedNames;
			Observation.NotRun = NotRunNames;
			Observation.InProcess = InProcessNames;
			Observation.Skipped = SkippedNames;

			const ClaireonTestRunStrict::FVerdict Verdict =
				ClaireonTestRunStrict::EvaluateExecution(Strict, TestFilter, Observation);

			if (Verdict.UnusedAllowances.Num() > 0)
			{
				Result += FString::Printf(
					TEXT("\nStrict: %d allowed_skips entr(y/ies) did not skip and can be retired: %s\n"),
					Verdict.UnusedAllowances.Num(), *FString::Join(Verdict.UnusedAllowances, TEXT(", ")));
			}

			if (Verdict.bFailed)
			{
				FString Message = Verdict.Message;
				Message += TEXT("\n");
				Message += Result;

				FToolResult StrictResult = MakeErrorResult(Message);
				StrictResult.Data = Verdict.Data;
				return StrictResult;
			}

			Result += FString::Printf(
				TEXT("Strict: PASSED -- %d discovered, %d reported, %d allowlisted skip(s) observed.\n"),
				DiscoveredTestNames.Num(), ExecutedNames.Num(), SkippedNames.Num());
		}

		const bool bIsError = (Failed > 0) || bTimedOut;
		if (bIsError)
		{
			return MakeErrorResult(Result);
		}
		return MakeSuccessResult(nullptr, Result);
	}();

	TestRun_Release(State);
	return Report;
}
