// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

// Evaluate strict test selection independently of automation execution, including
// missing-report outcomes that a healthy controller cannot deliberately produce.

#include "CoreMinimal.h"
#include "Containers/Map.h"
#include "Containers/Set.h"
#include "Templates/SharedPointer.h"

class FJsonObject;

namespace ClaireonTestRunStrict
{
	/** Per-call strict checks; disabled by default for compatibility. */
	struct FSpec
	{
		/** Whether strict checks are enabled. */
		bool bEnabled = false;

		/** expected_tests was supplied, so the discovery check runs. */
		bool bHasExpectedTests = false;

		/** The expected full test-NAME set. Names, never a count. */
		TSet<FString> ExpectedTests;

		/** allowed_skips was supplied. */
		bool bHasAllowedSkips = false;

		/** Individually named permitted skips, each with its reason. */
		TMap<FString, FString> AllowedSkips;
	};

	/** Everything observed after a run finished. Counts are derived from the name sets. */
	struct FExecutionObservation
	{
		/** How many leaf reports the controller produced. Zero is the vacuous-green case. */
		int32 ReportCount = 0;

		/** The run hit its deadline, so its results are incomplete by construction. */
		bool bTimedOut = false;

		/** The deadline that was in force, for the message. */
		double TimeoutSeconds = 0.0;

		/** Names discovered locally before the run. */
		TSet<FString> Discovered;

		/** Names that produced a report of any kind. */
		TSet<FString> Reported;

		TSet<FString> Failed;
		TSet<FString> NotRun;
		TSet<FString> InProcess;
		TSet<FString> Skipped;
	};

	/** Runner verdict with formatted error data and ordered findings. */
	struct FVerdict
	{
		bool bFailed = false;

		/** Fully formatted, ready to be a tool error message. */
		FString Message;

		/** Structured failure data. Never null after a failing evaluation. */
		TSharedPtr<FJsonObject> Data;

		/** One entry per finding, in evaluation order. Empty on a pass. */
		TArray<FString> Findings;

		/** Unused skip allowances are reported without failing the run. */
		TArray<FString> UnusedAllowances;
	};

	/**
	 * Parse `strict`, `expected_tests` and `allowed_skips`. False (with OutError set) on a
	 * malformed request; a missing argument object is not malformed.
	 */
	bool ParseSpec(const TSharedPtr<FJsonObject>& Arguments, FSpec& Out, FString& OutError);

	/**
	 * Compare discovered names with expected_tests before execution. Do not subtract
	 * skip allowances: an allowed skip must still refer to a discovered test.
	 */
	FVerdict EvaluateDiscovery(const FSpec& Spec, const FString& TestFilter, const TSet<FString>& Discovered);

	/**
	 * Require complete reporting without failures, pending tests, or timeout, and
	 * at least one report. Then validate skips against the allowlist.
	 */
	FVerdict EvaluateExecution(const FSpec& Spec, const FString& TestFilter, const FExecutionObservation& Observation);

	/** Sorted, for stable and diffable messages. Exposed because tests want the same order. */
	TArray<FString> Sorted(const TSet<FString>& In);
}
