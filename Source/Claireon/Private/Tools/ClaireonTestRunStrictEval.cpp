// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTestRunStrictEval.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonTestRunStrictInternal
{

	TSharedPtr<FJsonValue> StrictEval_StringArrayValue(const TArray<FString>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Reserve(In.Num());
		for (const FString& Item : In)
		{
			Values.Add(MakeShared<FJsonValueString>(Item));
		}
		return MakeShared<FJsonValueArray>(Values);
	}
}

using namespace ClaireonTestRunStrictInternal;

namespace ClaireonTestRunStrict
{
	TArray<FString> Sorted(const TSet<FString>& In)
	{
		TArray<FString> Out = In.Array();
		Out.Sort();
		return Out;
	}

	bool ParseSpec(const TSharedPtr<FJsonObject>& Arguments, FSpec& Out, FString& OutError)
	{
		if (!Arguments.IsValid())
		{
			return true;
		}

		bool bStrict = false;
		if (Arguments->TryGetBoolField(TEXT("strict"), bStrict))
		{
			Out.bEnabled = bStrict;
		}

		const TArray<TSharedPtr<FJsonValue>>* ExpectedArray = nullptr;
		if (Arguments->TryGetArrayField(TEXT("expected_tests"), ExpectedArray) && ExpectedArray != nullptr)
		{
			Out.bHasExpectedTests = true;
			Out.bEnabled = true;
			for (const TSharedPtr<FJsonValue>& Value : *ExpectedArray)
			{
				FString Name;
				if (!Value.IsValid() || !Value->TryGetString(Name) || Name.IsEmpty())
				{
					OutError = TEXT("expected_tests must be an array of non-empty test-path strings.");
					return false;
				}
				Out.ExpectedTests.Add(Name);
			}
			if (Out.ExpectedTests.Num() == 0)
			{
				OutError = TEXT("expected_tests was supplied but empty. An empty expected set can only ever "
					"describe a run that executes nothing, which is the exact outcome strict selection exists "
					"to reject; omit the parameter instead.");
				return false;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* SkipArray = nullptr;
		if (Arguments->TryGetArrayField(TEXT("allowed_skips"), SkipArray) && SkipArray != nullptr)
		{
			Out.bHasAllowedSkips = true;
			Out.bEnabled = true;
			for (const TSharedPtr<FJsonValue>& Value : *SkipArray)
			{
				const TSharedPtr<FJsonObject>* Entry = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(Entry) || Entry == nullptr || !(*Entry).IsValid())
				{
					OutError = TEXT("allowed_skips must be an array of {\"name\", \"reason\"} objects.");
					return false;
				}
				FString Name;
				FString Reason;
				(*Entry)->TryGetStringField(TEXT("name"), Name);
				(*Entry)->TryGetStringField(TEXT("reason"), Reason);
				if (Name.IsEmpty())
				{
					OutError = TEXT("every allowed_skips entry needs a non-empty 'name'.");
					return false;
				}
				if (Reason.IsEmpty())
				{
					OutError = FString::Printf(
						TEXT("allowed_skips entry '%s' has no 'reason'. An unexplained allowlisted skip is "
							 "indistinguishable from coverage that was quietly abandoned."), *Name);
					return false;
				}
				Out.AllowedSkips.Add(Name, Reason);
			}
		}

		// Skip allowances must belong to the expected test set when one is supplied.
		if (Out.bHasExpectedTests && Out.bHasAllowedSkips)
		{
			TArray<FString> Stray;
			for (const TPair<FString, FString>& Pair : Out.AllowedSkips)
			{
				if (!Out.ExpectedTests.Contains(Pair.Key))
				{
					Stray.Add(Pair.Key);
				}
			}
			if (Stray.Num() > 0)
			{
				Stray.Sort();
				OutError = FString::Printf(
					TEXT("allowed_skips names %d test(s) absent from expected_tests: %s. An allowlist entry "
						 "outside the expected set cannot be validated against anything."),
					Stray.Num(), *FString::Join(Stray, TEXT(", ")));
				return false;
			}
		}

		return true;
	}

	FVerdict EvaluateDiscovery(const FSpec& Spec, const FString& TestFilter, const TSet<FString>& Discovered)
	{
		FVerdict Verdict;
		if (!Spec.bHasExpectedTests)
		{
			return Verdict;
		}

		TSet<FString> Missing = Spec.ExpectedTests.Difference(Discovered);
		TSet<FString> Unexpected = Discovered.Difference(Spec.ExpectedTests);

		if (Missing.Num() == 0 && Unexpected.Num() == 0)
		{
			return Verdict;
		}

		const TArray<FString> MissingSorted = Sorted(Missing);
		const TArray<FString> UnexpectedSorted = Sorted(Unexpected);

		FString Message = FString::Printf(
			TEXT("Strict discovery FAILED for filter '%s': the discovered test-name set does not equal ")
			TEXT("expected_tests. Expected %d, discovered %d.\n"),
			*TestFilter, Spec.ExpectedTests.Num(), Discovered.Num());
		if (MissingSorted.Num() > 0)
		{
			Message += FString::Printf(TEXT("  MISSING (expected, not discovered): %s\n"),
				*FString::Join(MissingSorted, TEXT(", ")));
			Verdict.Findings.Add(FString::Printf(TEXT("MISSING: %s"), *FString::Join(MissingSorted, TEXT(", "))));
		}
		if (UnexpectedSorted.Num() > 0)
		{
			Message += FString::Printf(TEXT("  UNEXPECTED (discovered, not expected): %s\n"),
				*FString::Join(UnexpectedSorted, TEXT(", ")));
			Verdict.Findings.Add(FString::Printf(TEXT("UNEXPECTED: %s"), *FString::Join(UnexpectedSorted, TEXT(", "))));
		}
		Message += TEXT("No tests were run. This check deliberately precedes execution and ignores ")
			TEXT("allowed_skips, so an allowlisted test that has disappeared from the build fails here ")
			TEXT("rather than passing vacuously.");

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("strict_failure"), TEXT("discovery"));
		Data->SetField(TEXT("missing"), StrictEval_StringArrayValue(MissingSorted));
		Data->SetField(TEXT("unexpected"), StrictEval_StringArrayValue(UnexpectedSorted));
		Data->SetField(TEXT("discovered"), StrictEval_StringArrayValue(Sorted(Discovered)));

		Verdict.bFailed = true;
		Verdict.Message = MoveTemp(Message);
		Verdict.Data = Data;
		return Verdict;
	}

	FVerdict EvaluateExecution(const FSpec& Spec, const FString& TestFilter, const FExecutionObservation& Observation)
	{
		FVerdict Verdict;
		Verdict.Data = MakeShared<FJsonObject>();
		if (!Spec.bEnabled)
		{
			return Verdict;
		}

		// An empty report set satisfies all per-result counts; reject it explicitly.
		if (Observation.ReportCount == 0)
		{
			Verdict.Findings.Add(TEXT(
				"zero tests reported a result. This is the outcome the permissive default calls success: "
				"Failed, NotRun and InProcess are all trivially 0 when nothing ran."));
		}
		if (Observation.bTimedOut)
		{
			Verdict.Findings.Add(FString::Printf(
				TEXT("the run timed out after %.0fs, so its results are incomplete by construction."),
				Observation.TimeoutSeconds));
		}
		if (Observation.Failed.Num() > 0)
		{
			const TArray<FString> Names = Sorted(Observation.Failed);
			Verdict.Findings.Add(FString::Printf(TEXT("%d test(s) FAILED: %s"),
				Names.Num(), *FString::Join(Names, TEXT(", "))));
			Verdict.Data->SetField(TEXT("failed"), StrictEval_StringArrayValue(Names));
		}
		if (Observation.NotRun.Num() > 0)
		{
			const TArray<FString> Names = Sorted(Observation.NotRun);
			Verdict.Findings.Add(FString::Printf(TEXT("%d test(s) NEVER RAN: %s"),
				Names.Num(), *FString::Join(Names, TEXT(", "))));
			Verdict.Data->SetField(TEXT("not_run"), StrictEval_StringArrayValue(Names));
		}
		if (Observation.InProcess.Num() > 0)
		{
			const TArray<FString> Names = Sorted(Observation.InProcess);
			Verdict.Findings.Add(FString::Printf(TEXT("%d test(s) still IN PROGRESS: %s"),
				Names.Num(), *FString::Join(Names, TEXT(", "))));
			Verdict.Data->SetField(TEXT("in_process"), StrictEval_StringArrayValue(Names));
		}

		// Compare name sets to catch discovered tests absent from the reports.
		{
			const TSet<FString> Unreported = Observation.Discovered.Difference(Observation.Reported);
			if (Unreported.Num() > 0)
			{
				const TArray<FString> Names = Sorted(Unreported);
				Verdict.Findings.Add(FString::Printf(
					TEXT("%d discovered test(s) produced no report at all: %s"),
					Names.Num(), *FString::Join(Names, TEXT(", "))));
				Verdict.Data->SetField(TEXT("unreported"), StrictEval_StringArrayValue(Names));
			}
		}

		// Skip allowances apply only to execution outcomes.
		if (Observation.Skipped.Num() > 0)
		{
			TArray<FString> Unexpected;
			for (const FString& Name : Observation.Skipped)
			{
				if (!Spec.AllowedSkips.Contains(Name))
				{
					Unexpected.Add(Name);
				}
			}
			if (Unexpected.Num() > 0)
			{
				Unexpected.Sort();
				Verdict.Findings.Add(FString::Printf(
					TEXT("%d unexpected SKIP(s), absent from allowed_skips: %s. A skip is usually an "
						 "automation exclude-list entry; add it to allowed_skips with a reason, or remove "
						 "the exclusion."),
					Unexpected.Num(), *FString::Join(Unexpected, TEXT(", "))));
				Verdict.Data->SetField(TEXT("unexpected_skips"), StrictEval_StringArrayValue(Unexpected));
			}
			const TArray<FString> AllSkips = Sorted(Observation.Skipped);
			Verdict.Data->SetField(TEXT("skipped"), StrictEval_StringArrayValue(AllSkips));
		}

		// Report unused allowances without failing tests that no longer need them.
		for (const TPair<FString, FString>& Pair : Spec.AllowedSkips)
		{
			if (!Observation.Skipped.Contains(Pair.Key))
			{
				Verdict.UnusedAllowances.Add(Pair.Key);
			}
		}
		if (Verdict.UnusedAllowances.Num() > 0)
		{
			Verdict.UnusedAllowances.Sort();
			Verdict.Data->SetField(TEXT("unused_allowed_skips"),
				StrictEval_StringArrayValue(Verdict.UnusedAllowances));
		}

		if (Verdict.Findings.Num() > 0)
		{
			FString Message = FString::Printf(TEXT("Strict run FAILED for filter '%s' (%d finding(s)):\n"),
				*TestFilter, Verdict.Findings.Num());
			for (const FString& Finding : Verdict.Findings)
			{
				Message += FString::Printf(TEXT("  - %s\n"), *Finding);
			}
			Verdict.bFailed = true;
			Verdict.Message = MoveTemp(Message);
			Verdict.Data->SetStringField(TEXT("strict_failure"), TEXT("execution"));
			Verdict.Data->SetNumberField(TEXT("discovered_count"), Observation.Discovered.Num());
			Verdict.Data->SetNumberField(TEXT("reported_count"), Observation.Reported.Num());
		}

		return Verdict;
	}
}
