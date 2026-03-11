// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TestRun.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "IAutomationControllerModule.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/FilterCollection.h"
#include "Misc/Paths.h"

FString ClaireonTool_TestRun::GetName() const
{
	return TEXT("editor.test.run");
}

FString ClaireonTool_TestRun::GetDescription() const
{
	return TEXT("Run automation tests using the Unreal Automation Framework");
}

TSharedPtr<FJsonObject> ClaireonTool_TestRun::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// testFilter (optional)
	TSharedPtr<FJsonObject> FilterProp = MakeShared<FJsonObject>();
	FilterProp->SetStringField(TEXT("type"), TEXT("string"));
	FilterProp->SetStringField(TEXT("description"),
		TEXT("Filter pattern to select specific tests (e.g. 'Combat', 'Ability.Fire')"));
	Properties->SetObjectField(TEXT("testFilter"), FilterProp);

	// noTimeout (optional)
	TSharedPtr<FJsonObject> NoTimeoutProp = MakeShared<FJsonObject>();
	NoTimeoutProp->SetStringField(TEXT("type"), TEXT("boolean"));
	NoTimeoutProp->SetStringField(TEXT("description"),
		TEXT("Disable test timeout (useful for debugging, default: false)"));
	Properties->SetObjectField(TEXT("noTimeout"), NoTimeoutProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_TestRun::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse optional arguments
	FString TestFilter;
	bool bNoTimeout = false;

	if (Arguments.IsValid())
	{
		Arguments->TryGetStringField(TEXT("testFilter"), TestFilter);
		Arguments->TryGetBoolField(TEXT("noTimeout"), bNoTimeout);
	}

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] editor.test.run: testFilter='%s', noTimeout=%s"),
		*TestFilter, bNoTimeout ? TEXT("true") : TEXT("false"));

	if (TestFilter.IsEmpty())
	{
		return MakeErrorResult(TEXT("testFilter is required to avoid running all tests accidentally."));
	}

	// Step 1: Find matching tests from the local automation framework
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

	// Step 2: Get the automation controller and set up for running
	IAutomationControllerModule& AutomationModule =
		FModuleManager::LoadModuleChecked<IAutomationControllerModule>(TEXT("AutomationController"));
	IAutomationControllerManagerRef Controller = AutomationModule.GetAutomationController();

	Controller->Init();

	// Request available workers (the editor itself is a worker)
	Controller->RequestAvailableWorkers(FApp::GetSessionId());

	// Step 3: Wait for workers to become available, ticking the controller
	const double StartTime = FPlatformTime::Seconds();
	constexpr double WorkerTimeoutSeconds = 30.0;
	constexpr double RunTimeoutSeconds = 1800.0;

	// Poll until the controller has device clusters (workers available)
	while (Controller->GetNumDeviceClusters() == 0)
	{
		AutomationModule.Tick();
		FPlatformProcess::Sleep(0.1f);

		const double Elapsed = FPlatformTime::Seconds() - StartTime;
		if (Elapsed > WorkerTimeoutSeconds)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Timed out after %.0fs waiting for automation workers. The editor automation worker may not be running."),
				WorkerTimeoutSeconds));
		}
	}

	// Step 4: Request tests from workers and wait for them to arrive
	Controller->RequestTests();

	// Wait for test list to be populated by ticking
	const double RequestStartTime = FPlatformTime::Seconds();
	constexpr double RequestTimeoutSeconds = 30.0;
	bool bTestsAvailable = false;

	// We need to tick and wait for the OnTestsRefreshed callback to fire internally,
	// which populates the report tree. We detect this by checking GetFilteredTestNames.
	for (;;)
	{
		AutomationModule.Tick();
		FPlatformProcess::Sleep(0.1f);

		TArray<FString> AvailableNames;
		Controller->GetFilteredTestNames(AvailableNames);
		if (AvailableNames.Num() > 0)
		{
			bTestsAvailable = true;
			break;
		}

		const double Elapsed = FPlatformTime::Seconds() - RequestStartTime;
		if (Elapsed > RequestTimeoutSeconds)
		{
			break;
		}
	}

	if (!bTestsAvailable)
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[MCP] No tests reported by automation workers within %.0fs. Proceeding with local test names."),
			RequestTimeoutSeconds);
	}

	// Step 5: Enable only our matching tests and run
	Controller->SetVisibleTestsEnabled(false);
	Controller->SetEnabledTests(MatchingTestNames);

	const int32 EnabledCount = Controller->GetEnabledTestsNum();
	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] Enabled %d tests for execution (requested %d)"), EnabledCount, MatchingTestNames.Num());

	if (EnabledCount == 0)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("No tests were enabled for execution. Found %d local matches for filter '%s', but the automation controller could not match them to runnable tests. ")
				TEXT("This can happen if the automation worker has not fully registered all tests yet."),
			MatchingTestNames.Num(), *TestFilter));
	}

	Controller->SetNumPasses(1);
	Controller->RunTests(/* bIsLocalSession */ true);

	// Step 6: Poll for completion
	const double RunStartTime = FPlatformTime::Seconds();
	const double EffectiveTimeout = bNoTimeout ? TNumericLimits<double>::Max() : RunTimeoutSeconds;
	bool bTimedOut = false;

	while (Controller->GetTestState() == EAutomationControllerModuleState::Running)
	{
		AutomationModule.Tick();
		FPlatformProcess::Sleep(0.5f);

		const double Elapsed = FPlatformTime::Seconds() - RunStartTime;
		if (Elapsed > EffectiveTimeout)
		{
			bTimedOut = true;
			Controller->StopTests();
			UE_LOG(LogClaireon, Warning,
				TEXT("[MCP] Test execution timed out after %.0fs, stopping tests"), EffectiveTimeout);
			break;
		}
	}

	const double TotalDuration = FPlatformTime::Seconds() - StartTime;

	// Step 7: Collect results from the reports
	TArray<TSharedPtr<IAutomationReport>> EnabledReports = Controller->GetEnabledReports();

	int32 Passed = 0;
	int32 Failed = 0;
	int32 Skipped = 0;
	int32 NotRun = 0;
	int32 InProcess = 0;

	FString DetailedResults;

	for (const TSharedPtr<IAutomationReport>& Report : EnabledReports)
	{
		if (!Report.IsValid())
		{
			continue;
		}

		const FString& TestDisplayName = Report->GetDisplayName();

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
				StateStr = TEXT("FAIL");
				break;
			case EAutomationState::Skipped:
				Skipped++;
				StateStr = TEXT("SKIP");
				break;
			case EAutomationState::InProcess:
				InProcess++;
				StateStr = TEXT("IN_PROGRESS");
				break;
			case EAutomationState::NotRun:
			default:
				NotRun++;
				StateStr = TEXT("NOT_RUN");
				break;
		}

		DetailedResults += FString::Printf(TEXT("  [%s] %s\n"), *StateStr, *TestDisplayName);

		// For failed tests, include error messages
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

	// Step 8: Build result string
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

	const bool bIsError = (Failed > 0) || bTimedOut;
	if (bIsError)
	{
		return MakeErrorResult(Result);
	}
	return MakeSuccessResult(nullptr, Result);
}
