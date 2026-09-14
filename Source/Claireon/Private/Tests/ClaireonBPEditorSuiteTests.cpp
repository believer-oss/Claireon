// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Interactive editor tests under Claireon.BPEditor, run through test_run or Test Automation.
// They require Slate and a real transaction buffer.
// Unique substrate fixtures use the configured deletion helper; downstream deletion may
// still scan referencers. The saved BlueprintAssist fixture remains open across runs
// and requires separate cleanup.

#include "Misc/AutomationTest.h"

#if WITH_CLAIREON_TESTS && WITH_EDITOR

#include "ClaireonSessionManager.h"
#include "Tests/ClaireonBASettleHelper.h"
#include "Tests/ClaireonBPEditorFixtures.h"
#include "Tests/ClaireonTestAssetDeletion.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Tools/ClaireonTestRunStrictEval.h"
#include "Tools/ClaireonTool_TransactionHistory.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"
#include "Engine/Blueprint.h"
#include "Framework/Application/SlateApplication.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/IToolkit.h"
#include "Toolkits/IToolkitHost.h"

namespace ClaireonBPEditorSuiteInternal
{
	// Prefix helpers to avoid unity-build collisions.

	/**
	 * Keep the shared BA fixture and editor across calls so a real engine frame can create its handler.
	 * Cleanup is out of band.
	 */
	static FString BPE_SettleFixturePath()
	{
		return ClaireonBPEditorFixtures::SharedBlueprintAssistFixturePath();
	}
}

// Verify the live editor transaction buffer is exposed by transaction_history.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorTransactionBufferIsReal,
	"Claireon.BPEditor.Substrate.TransactionBufferIsReal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonBPEditorTransactionBufferIsReal::RunTest(const FString& /*Parameters*/)
{
	if (!IsValid(GEditor))
	{
		AddError(TEXT("GEditor is null; this suite is EditorContext-only and must not have been discovered here."));
		return false;
	}

	UTransBuffer* TransBuffer = Cast<UTransBuffer>(GEditor->Trans);
	if (!IsValid(TransBuffer))
	{
		AddError(TEXT("GEditor->Trans is not a UTransBuffer. Every transaction assertion in this package "
			"depends on it, so this is a hard stop rather than a skip."));
		return false;
	}

	ClaireonTool_TransactionHistory HistoryTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	const IClaireonTool::FToolResult Result = HistoryTool.Execute(Args);

	if (Result.bIsError)
	{
		AddError(FString::Printf(
			TEXT("transaction_history returned its error path in an interactive editor: %s"),
			*Result.ErrorMessage));
		return false;
	}
	if (!Result.Data.IsValid())
	{
		AddError(TEXT("transaction_history succeeded with no structured data."));
		return false;
	}

	double TotalInBuffer = -1.0;
	if (!Result.Data->TryGetNumberField(TEXT("total_in_buffer"), TotalInBuffer))
	{
		AddError(TEXT("transaction_history omitted total_in_buffer."));
		return false;
	}
	TestTrue(TEXT("total_in_buffer is a real count"), TotalInBuffer >= 0.0);
	TestEqual(TEXT("the tool's count agrees with the buffer it read"),
		static_cast<int32>(TotalInBuffer), TransBuffer->GetQueueLength());

	return true;
}

// Verify tests can invoke session-based tools.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorSessionToolsAreInvocable,
	"Claireon.BPEditor.Substrate.SessionToolsAreInvocable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonBPEditorSessionToolsAreInvocable::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorSuiteInternal;

	const FString AssetPath = ClaireonBPEditorFixtures::UniquePath(TEXT("Session"));
	FString CreateError;
	UBlueprint* Blueprint = ClaireonBPEditorFixtures::Create(AssetPath, CreateError);
	if (!IsValid(Blueprint))
	{
		AddError(FString::Printf(TEXT("fixture creation failed: %s"), *CreateError));
		ClaireonBPEditorFixtures::Teardown(AssetPath);
		return false;
	}

	ClaireonBlueprintGraphTool_Open OpenTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	const IClaireonTool::FToolResult Opened = OpenTool.Execute(Args);

	if (Opened.bIsError || !Opened.Data.IsValid())
	{
		AddError(FString::Printf(TEXT("bp_open failed on an in-memory fixture: %s"), *Opened.ErrorMessage));
		ClaireonBPEditorFixtures::Teardown(AssetPath);
		return false;
	}

	FString SessionId;
	Opened.Data->TryGetStringField(TEXT("session_id"), SessionId);
	TestFalse(TEXT("bp_open returned a session id"), SessionId.IsEmpty());

	TestNotNull(TEXT("the fixture has an ubergraph to address"), ClaireonBPEditorFixtures::FirstUbergraph(Blueprint));

	ClaireonBPEditorFixtures::Teardown(AssetPath);
	return true;
}

// Exercise fixture creation and the configured cleanup path.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorFixtureLifecycleIsScanFree,
	"Claireon.BPEditor.Substrate.FixtureLifecycleIsScanFree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonBPEditorFixtureLifecycleIsScanFree::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorSuiteInternal;

	const FString AssetPath = ClaireonBPEditorFixtures::UniquePath(TEXT("Lifecycle"));
	FString CreateError;
	UBlueprint* Blueprint = ClaireonBPEditorFixtures::Create(AssetPath, CreateError);
	if (!IsValid(Blueprint))
	{
		AddError(FString::Printf(TEXT("fixture creation failed: %s"), *CreateError));
		return false;
	}

	// CreateBlueprint registers a dirty in-memory asset without saving it.
	TestNotNull(TEXT("the fixture resolved before deletion"), ClaireonBPEditorFixtures::Resolve(AssetPath));

	FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);
	const int32 Deleted = ClaireonTestAssetDeletion::DeleteObjectsForTest({ Blueprint });
	TestEqual(TEXT("the scan-free delete removed exactly the fixture"), Deleted, 1);
	TestNull(TEXT("the fixture no longer resolves"), ClaireonBPEditorFixtures::Resolve(AssetPath));

	return true;
}

// Verify settlement drives the intended graph. A missing handler prepares the shared editor and fails
// with instructions to retry after an engine frame; creation and opening use separate updates.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorSettlementDrivesIntendedGraph,
	"Claireon.BPEditor.BlueprintAssist.SettlementDrivesTheIntendedGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorSuiteInternal
{
	/**
	 * Try to settle; if there is no handler yet, prepare the fixture and its editor so
	 * the next invocation has one, and fail with that instruction.
	 */
	class FBPE_SettleLatentCommand : public IAutomationLatentCommand
	{
	public:
		FBPE_SettleLatentCommand(FAutomationTestBase* InTest, double InSettleBudgetSeconds)
			: Test(InTest)
			, SettleBudgetSeconds(InSettleBudgetSeconds)
		{
		}

		virtual bool Update() override
		{
			switch (Phase)
			{
			case EPhase::TrySettle:
			{
				UEdGraph* Graph = ClaireonBPEditorFixtures::FirstUbergraph(
					Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(BPE_SettleFixturePath())));
				if (IsValid(Graph))
				{
					ClaireonBASettleHelper::FSettleOptions Options;
					Options.HandlerTimeoutSeconds = 2.0;
					const ClaireonBASettleHelper::FSettleReport Report =
						ClaireonBASettleHelper::Settle(Graph, Options);

					if (Report.Settled())
					{
						Test->TestTrue(TEXT("the settle drove the graph the test asked for"),
							Report.bIntendedGraphConfirmed);
						Test->TestTrue(TEXT("the settle actually pumped BlueprintAssist"),
							Report.PumpIterations > 0);
						Test->TestTrue(TEXT("node sizes finished measuring"),
							Report.bNodeSizesSettled);
						Test->AddInfo(FString::Printf(TEXT("settled: %s"), *Report.Diagnostics));
						return true;
					}

					if (!Report.bBlueprintAssistCompiledIn || !Report.bSlateAvailable)
					{
						Test->AddError(Report.Diagnostics);
						return true;
					}

					LastDiagnostics = Report.Diagnostics;
				}
				else
				{
					LastDiagnostics = TEXT("fixture absent");
				}

				if (GetCurrentRunTime() > SettleBudgetSeconds)
				{
					Phase = EPhase::Prepare;
				}
				return false;
			}

			case EPhase::Prepare:
			{
				const FString AssetPath = BPE_SettleFixturePath();
				if (!IsValid(ClaireonBPEditorFixtures::Resolve(AssetPath)))
				{
					FString CreateError;
					UBlueprint* Created = ClaireonBPEditorFixtures::Create(AssetPath, CreateError);
					if (!IsValid(Created))
					{
						Test->AddError(FString::Printf(
							TEXT("could not create the settlement fixture at %s: %s"),
							*AssetPath, *CreateError));
						return true;
					}
					// Save before opening the editor in a later update.
					if (!ClaireonBPEditorFixtures::Save(Created))
					{
						Test->AddWarning(FString::Printf(
							TEXT("the settlement fixture at %s could not be saved; it will be left as an "
								 "unsaved dirty package, which raises the Restore-Packages modal on the "
								 "next editor launch."), *AssetPath));
					}
					PreparedNote = TEXT("Created and saved the fixture");
					Phase = EPhase::Idle;
					return false;
				}
				Phase = EPhase::Open;
				return false;
			}

			case EPhase::Idle:
				if (++IdleUpdates < 3)
				{
					return false;
				}
				Phase = EPhase::Open;
				return false;

			case EPhase::Open:
			{
				UObject* Asset = ClaireonBPEditorFixtures::Resolve(BPE_SettleFixturePath());
				UAssetEditorSubsystem* Subsystem = IsValid(GEditor)
					? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()
					: nullptr;
				if (!IsValid(Asset) || !IsValid(Subsystem))
				{
					Test->AddError(TEXT("the settlement fixture or the asset-editor subsystem went away."));
					return true;
				}

				// Open synchronously without a progress dialog.
				Subsystem->OpenEditorForAsset(Asset, EToolkitMode::Standalone,
					TSharedPtr<IToolkitHost>(), /*bShowProgressWindow*/ false);

				// Queue the graph tab for handler creation on the next real frame.
				if (UEdGraph* Graph = ClaireonBPEditorFixtures::FirstUbergraph(
						Cast<UBlueprint>(ClaireonBPEditorFixtures::Resolve(BPE_SettleFixturePath()))); IsValid(Graph))
				{
					ClaireonBASettleHelper::FSettleOptions Options;
					Options.HandlerTimeoutSeconds = 2.0;
					const ClaireonBASettleHelper::FSettleReport Report =
						ClaireonBASettleHelper::Settle(Graph, Options);
					LastDiagnostics = Report.Diagnostics;
					if (Report.Settled())
					{
						Test->TestTrue(TEXT("the settle drove the graph the test asked for"),
							Report.bIntendedGraphConfirmed);
						Test->AddInfo(FString::Printf(TEXT("settled on the preparing run: %s"),
							*Report.Diagnostics));
						return true;
					}
				}

				Test->AddError(FString::Printf(
					TEXT("PREPARED, NOT SETTLED. %s and opened its editor at %s, and activated and ")
					TEXT("enqueued its graph tab. BlueprintAssist cannot build a graph handler inside a ")
					TEXT("tool call: its handler comes from a SetTimerForNextTick timer, and ")
					TEXT("FTimerManager::Tick early-returns for the rest of a frame once ")
					TEXT("UEditorEngine::Tick has ticked it -- which it always has by the time MCP ")
					TEXT("dispatch runs -- while GFrameCounter cannot advance during this call. ")
					TEXT("RE-RUN this suite: the intervening engine frames build the handler and the ")
					TEXT("second run settles. The fixture is deliberately left in place under the ")
					TEXT("gitignored /Game/_ClaireonBPEditor/ for exactly that reason. Last settle: %s"),
					PreparedNote.IsEmpty() ? TEXT("Reused the existing fixture") : *PreparedNote,
					*BPE_SettleFixturePath(), *LastDiagnostics));
				return true;
			}

			default:
				return true;
			}
		}

	private:
		enum class EPhase : uint8
		{
			TrySettle,
			Prepare,
			Idle,
			Open
		};

		FAutomationTestBase* Test = nullptr;
		FString LastDiagnostics;
		FString PreparedNote;
		double SettleBudgetSeconds = 10.0;
		EPhase Phase = EPhase::TrySettle;
		int32 IdleUpdates = 0;
	};
}

bool FClaireonBPEditorSettlementDrivesIntendedGraph::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorSuiteInternal;

	if (!FSlateApplication::IsInitialized())
	{
		AddError(TEXT("FSlateApplication is not initialized. This suite is EditorContext-only so that "
			"cannot happen through the commandlet; reaching it means the suite was scheduled "
			"somewhere it cannot assert anything."));
		return false;
	}

	ADD_LATENT_AUTOMATION_COMMAND(FBPE_SettleLatentCommand(this, 10.0));
	return true;
}


// Test strict-run judgments directly, including discovery followed by zero results.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonBPEditorStrictSelectionRejectsVacuousRuns,
	"Claireon.BPEditor.Runner.StrictSelectionRejectsVacuousRuns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace ClaireonBPEditorSuiteInternal
{
	static bool BPE_AnyFindingContains(const ClaireonTestRunStrict::FVerdict& Verdict, const TCHAR* Needle)
	{
		for (const FString& Finding : Verdict.Findings)
		{
			if (Finding.Contains(Needle))
			{
				return true;
			}
		}
		return false;
	}

	static FString BPE_StrictFailureField(const ClaireonTestRunStrict::FVerdict& Verdict)
	{
		FString Value;
		if (Verdict.Data.IsValid())
		{
			Verdict.Data->TryGetStringField(TEXT("strict_failure"), Value);
		}
		return Value;
	}
}

bool FClaireonBPEditorStrictSelectionRejectsVacuousRuns::RunTest(const FString& /*Parameters*/)
{
	using namespace ClaireonBPEditorSuiteInternal;
	using namespace ClaireonTestRunStrict;

	const FString Filter = TEXT("Claireon.BPEditor");
	const FString NameA = TEXT("Claireon.BPEditor.Substrate.TransactionBufferIsReal");
	const FString NameGhost = TEXT("Claireon.BPEditor.Substrate.DeletedFromTheBuild");

	// ---- Parse: an empty expected set describes only a run that executes nothing.
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetArrayField(TEXT("expected_tests"), TArray<TSharedPtr<FJsonValue>>());
		FSpec Spec;
		FString Error;
		TestFalse(TEXT("expected_tests=[] is rejected"), ParseSpec(Args, Spec, Error));
		TestTrue(TEXT("and says why"), Error.Contains(TEXT("empty expected set")));
	}

	// ---- Parse: an allowlisted skip with no stated reason is how coverage is abandoned.
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), NameA);
		TArray<TSharedPtr<FJsonValue>> Skips;
		Skips.Add(MakeShared<FJsonValueObject>(Entry));
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetArrayField(TEXT("allowed_skips"), Skips);
		FSpec Spec;
		FString Error;
		TestFalse(TEXT("an allowlisted skip without a reason is rejected"), ParseSpec(Args, Spec, Error));
		TestTrue(TEXT("and says why"), Error.Contains(TEXT("reason")));
	}

	// ---- Parse: an absent argument object leaves the permissive default intact.
	{
		FSpec Spec;
		FString Error;
		TestTrue(TEXT("an absent argument object parses"), ParseSpec(nullptr, Spec, Error));
		TestFalse(TEXT("and leaves strictness off, preserving the permissive default"), Spec.bEnabled);
	}

	// ---- Parse: supplying either array implies strict.
	{
		TArray<TSharedPtr<FJsonValue>> Expected;
		Expected.Add(MakeShared<FJsonValueString>(NameA));
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetArrayField(TEXT("expected_tests"), Expected);
		FSpec Spec;
		FString Error;
		TestTrue(TEXT("expected_tests parses"), ParseSpec(Args, Spec, Error));
		TestTrue(TEXT("and implies strict"), Spec.bEnabled);
	}

	// ---- Discovery: a name expected but not discovered fails, and is NAMED.
	{
		FSpec Spec;
		Spec.bEnabled = true;
		Spec.bHasExpectedTests = true;
		Spec.ExpectedTests = { NameA, NameGhost };
		const TSet<FString> Discovered = { NameA };

		const FVerdict Verdict = EvaluateDiscovery(Spec, Filter, Discovered);
		TestTrue(TEXT("a missing expected test fails discovery"), Verdict.bFailed);
		TestEqual(TEXT("and is reported as a discovery failure"),
			BPE_StrictFailureField(Verdict), FString(TEXT("discovery")));
		TestTrue(TEXT("and the missing name is in the message, not just a count"),
			Verdict.Message.Contains(NameGhost));
	}

	// An allowlisted skip must still be discovered.
	{
		FSpec Spec;
		Spec.bEnabled = true;
		Spec.bHasExpectedTests = true;
		Spec.ExpectedTests = { NameA, NameGhost };
		Spec.bHasAllowedSkips = true;
		Spec.AllowedSkips.Add(NameGhost, TEXT("allowlisted, and absent from the build"));
		const TSet<FString> Discovered = { NameA };

		const FVerdict Verdict = EvaluateDiscovery(Spec, Filter, Discovered);
		TestTrue(TEXT("an allowlisted-but-undiscovered test still fails discovery"), Verdict.bFailed);
		TestTrue(TEXT("and is still named"), Verdict.Message.Contains(NameGhost));
	}

	// Compare names, not counts, to catch unexpected tests.
	{
		FSpec Spec;
		Spec.bEnabled = true;
		Spec.bHasExpectedTests = true;
		Spec.ExpectedTests = { NameA };
		const TSet<FString> Discovered = { NameGhost };

		const FVerdict Verdict = EvaluateDiscovery(Spec, Filter, Discovered);
		TestTrue(TEXT("a one-for-one rename fails even though the count matches"), Verdict.bFailed);
		TestTrue(TEXT("naming the unexpected test"), Verdict.Message.Contains(NameGhost));
		TestTrue(TEXT("and the missing one"), Verdict.Message.Contains(NameA));
	}

	// ---- Discovery: set equality passes.
	{
		FSpec Spec;
		Spec.bEnabled = true;
		Spec.bHasExpectedTests = true;
		Spec.ExpectedTests = { NameA };
		const TSet<FString> Discovered = { NameA };
		TestFalse(TEXT("an equal name set passes discovery"),
			EvaluateDiscovery(Spec, Filter, Discovered).bFailed);
	}

	// Discovered tests with zero reported results must fail strict evaluation.
	{
		FSpec Spec;
		Spec.bEnabled = true;
		FExecutionObservation Observation;
		Observation.ReportCount = 0;
		Observation.Discovered = { NameA };

		const FVerdict Verdict = EvaluateExecution(Spec, Filter, Observation);
		TestTrue(TEXT("a run that discovered tests and reported nothing FAILS"), Verdict.bFailed);
		TestTrue(TEXT("naming the vacuous-green case"),
			BPE_AnyFindingContains(Verdict, TEXT("zero tests reported a result")));
		TestTrue(TEXT("and the discovered test is reported as unreported"),
			BPE_AnyFindingContains(Verdict, TEXT("produced no report at all")));
		TestEqual(TEXT("reported as an execution failure"),
			BPE_StrictFailureField(Verdict), FString(TEXT("execution")));
	}

	// ---- Execution: a timeout is incomplete by construction, not a pass.
	{
		FSpec Spec;
		Spec.bEnabled = true;
		FExecutionObservation Observation;
		Observation.ReportCount = 1;
		Observation.Discovered = { NameA };
		Observation.Reported = { NameA };
		Observation.bTimedOut = true;
		Observation.TimeoutSeconds = 1800.0;

		TestTrue(TEXT("a timed-out run fails"), EvaluateExecution(Spec, Filter, Observation).bFailed);
	}

	// ---- Execution: an unexpected skip fails; the same skip allowlisted does not.
	{
		FSpec Spec;
		Spec.bEnabled = true;
		FExecutionObservation Observation;
		Observation.ReportCount = 1;
		Observation.Discovered = { NameA };
		Observation.Reported = { NameA };
		Observation.Skipped = { NameA };

		const FVerdict Unexpected = EvaluateExecution(Spec, Filter, Observation);
		TestTrue(TEXT("an unexpected skip fails"), Unexpected.bFailed);
		TestTrue(TEXT("and is named"), Unexpected.Message.Contains(NameA));

		Spec.bHasAllowedSkips = true;
		Spec.AllowedSkips.Add(NameA, TEXT("allowlisted with a reason"));
		const FVerdict Allowed = EvaluateExecution(Spec, Filter, Observation);
		TestFalse(TEXT("the same skip, allowlisted with a reason, passes"), Allowed.bFailed);
		TestEqual(TEXT("and the allowance is not reported as stale"), Allowed.UnusedAllowances.Num(), 0);
	}

	// ---- Execution: an allowance whose test did not skip is reported, never failed.
	{
		FSpec Spec;
		Spec.bEnabled = true;
		Spec.bHasAllowedSkips = true;
		Spec.AllowedSkips.Add(NameA, TEXT("no longer needed"));
		FExecutionObservation Observation;
		Observation.ReportCount = 1;
		Observation.Discovered = { NameA };
		Observation.Reported = { NameA };

		const FVerdict Verdict = EvaluateExecution(Spec, Filter, Observation);
		TestFalse(TEXT("a stale allowance does not turn the run red"), Verdict.bFailed);
		TestEqual(TEXT("but it is surfaced for retirement"), Verdict.UnusedAllowances.Num(), 1);
	}

	// With strict mode disabled, these observations produce no verdict.
	{
		FSpec Spec;
		Spec.bEnabled = false;
		FExecutionObservation Observation;
		Observation.ReportCount = 0;
		Observation.Discovered = { NameA };
		Observation.Failed = { NameA };

		const FVerdict Verdict = EvaluateExecution(Spec, Filter, Observation);
		TestFalse(TEXT("with strict off, the evaluator judges nothing"), Verdict.bFailed);
		TestEqual(TEXT("and produces no findings"), Verdict.Findings.Num(), 0);
	}

	// ---- Execution: the healthy run passes.
	{
		FSpec Spec;
		Spec.bEnabled = true;
		FExecutionObservation Observation;
		Observation.ReportCount = 2;
		Observation.Discovered = { NameA, NameGhost };
		Observation.Reported = { NameA, NameGhost };

		TestFalse(TEXT("a complete run with no failures passes"),
			EvaluateExecution(Spec, Filter, Observation).bFailed);
	}

	return true;
}

#endif // WITH_CLAIREON_TESTS && WITH_EDITOR
