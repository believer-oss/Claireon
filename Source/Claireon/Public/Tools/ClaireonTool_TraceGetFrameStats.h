// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Frame-duration aggregation, split out of Execute so it can be tested without
 * a capture (P0-6a).
 *
 * An open frame carries EndTime = +inf, so its duration is non-finite. Summing
 * that poisons avg_ms outright and max_ms whenever the value is +inf, turning a
 * truncated capture into a confident wrong reading rather than a visible error.
 * The aggregate now skips non-finite frames and reports which ones it skipped.
 */
namespace ClaireonTraceFrameStats
{
	/** One frame's raw measurement, before any finiteness filtering. */
	struct FFrameSample
	{
		int32 FrameIndex = 0;
		double DurationMs = 0.0;
	};

	/** Aggregate over the finite frames only, plus the indices left out. */
	struct FFrameAggregate
	{
		double AvgMs = 0.0;
		double MinMs = 0.0;
		double MaxMs = 0.0;
		int32 FiniteFrameCount = 0;
		TArray<int32> UnterminatedFrameIndices;
	};

	FFrameAggregate AggregateFrameSamples(const TArray<FFrameSample>& Samples);
}

class ClaireonTool_TraceGetFrameStats : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
