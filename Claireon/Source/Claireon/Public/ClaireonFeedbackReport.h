// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

DECLARE_DELEGATE_TwoParams(FOnFeedbackReportComplete, bool /*bSuccess*/, const FString& /*Message*/);

/**
 * Generates a feedback report by aggregating MCP feedback entries and Python
 * audit log data, then sending them to Claude Opus for analysis.
 *
 * The report is saved to Saved/MCP/FeedbackReports/ and copied to clipboard
 * with the file path as the first line.
 */
class FClaireonFeedbackReport
{
public:
	/**
	 * Generate a feedback report asynchronously.
	 * @param OnComplete Called when generation finishes (success or failure).
	 */
	static void Generate(FOnFeedbackReportComplete OnComplete);

private:
	/** Aggregate feedback entries from Saved/MCP/Feedback/ */
	static FString AggregateFeedbackEntries(int32 MaxEntries = 50);

	/** Aggregate Python audit log entries */
	static FString AggregatePythonAuditEntries(int32 MaxEntries = 100);

	/** Build the prompt for Opus analysis */
	static FString BuildAnalysisPrompt(const FString& FeedbackData, const FString& PythonAuditData);

	/** Format the final report */
	static FString FormatReport(const FString& OpusAnalysis, const FString& FeedbackData);

	/** Get the output directory */
	static FString GetReportDir();

	/** Generate a timestamped filename */
	static FString GenerateReportFilename();
};
