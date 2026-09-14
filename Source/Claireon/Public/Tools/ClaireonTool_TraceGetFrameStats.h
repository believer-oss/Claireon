// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Aggregate finite frame durations and report skipped indices. Open frames have
 * an infinite end time, which would otherwise corrupt averages and maxima.
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
