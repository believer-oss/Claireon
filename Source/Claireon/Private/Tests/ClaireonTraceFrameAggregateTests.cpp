// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonTool_TraceGetFrameStats.h"

#include <limits>

// ---------------------------------------------------------------------------
// P0-6a: frame-stat aggregates were poisoned by unterminated frames.
//
// The engine seeds every open frame with EndTime = +inf, so a capture stopped
// mid-frame yields a non-finite duration. Fed into the running sum unguarded:
//   * avg_ms was completely poisoned by any non-finite frame
//   * max_ms was poisoned by +inf (the realistic case)
//   * min_ms was never affected -- +inf never wins a minimum
// so the reported numbers looked ordinary and were wrong.
//
// A real capture cannot be made to contain an open frame on demand, which is
// why the aggregation was extracted from Execute: these drive it directly.
// ---------------------------------------------------------------------------
namespace ClaireonTraceFrameAggregateTestsInternal
{

// File-local discriminator prefix (FrameAggregate_) per module convention.

double FrameAggregate_PosInf()
{
	return std::numeric_limits<double>::infinity();
}
double FrameAggregate_NaN()
{
	return std::numeric_limits<double>::quiet_NaN();
}

TArray<ClaireonTraceFrameStats::FFrameSample> FrameAggregate_MakeSamples(const TArray<double>& DurationsMs)
{
	TArray<ClaireonTraceFrameStats::FFrameSample> Samples;
	Samples.Reserve(DurationsMs.Num());
	for (int32 Index = 0; Index < DurationsMs.Num(); ++Index)
	{
		Samples.Add(ClaireonTraceFrameStats::FFrameSample{ Index, DurationsMs[Index] });
	}
	return Samples;
}

} // namespace ClaireonTraceFrameAggregateTestsInternal

using namespace ClaireonTraceFrameAggregateTestsInternal;

UNTEST_UNIT_OPTS(Claireon, TraceFrameAggregate, UnterminatedFrameExcludedFromAggregate, UNTEST_TIMEOUTMS(30000))
{
	// The canonical case: one open frame between two healthy ones.
	const TArray<ClaireonTraceFrameStats::FFrameSample> Samples =
		FrameAggregate_MakeSamples({ 16.6, FrameAggregate_PosInf(), 16.7 });

	const ClaireonTraceFrameStats::FFrameAggregate Aggregate =
		ClaireonTraceFrameStats::AggregateFrameSamples(Samples);

	UNTEST_EXPECT_EQ(Aggregate.FiniteFrameCount, 2);
	UNTEST_EXPECT_NEAR(Aggregate.AvgMs, 16.65, 0.0001);
	UNTEST_EXPECT_NEAR(Aggregate.MinMs, 16.6, 0.0001);
	// Was +inf before the fix -- the visible half of the defect.
	UNTEST_EXPECT_NEAR(Aggregate.MaxMs, 16.7, 0.0001);

	UNTEST_ASSERT_EQ(Aggregate.UnterminatedFrameIndices.Num(), 1);
	UNTEST_EXPECT_EQ(Aggregate.UnterminatedFrameIndices[0], 1);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, TraceFrameAggregate, NaNFrameExcludedFromAggregate, UNTEST_TIMEOUTMS(30000))
{
	const TArray<ClaireonTraceFrameStats::FFrameSample> Samples =
		FrameAggregate_MakeSamples({ 16.6, FrameAggregate_NaN(), 16.7 });

	const ClaireonTraceFrameStats::FFrameAggregate Aggregate =
		ClaireonTraceFrameStats::AggregateFrameSamples(Samples);

	UNTEST_EXPECT_EQ(Aggregate.FiniteFrameCount, 2);
	UNTEST_EXPECT_NEAR(Aggregate.AvgMs, 16.65, 0.0001);
	UNTEST_EXPECT_NEAR(Aggregate.MinMs, 16.6, 0.0001);
	UNTEST_EXPECT_NEAR(Aggregate.MaxMs, 16.7, 0.0001);
	UNTEST_EXPECT_EQ(Aggregate.UnterminatedFrameIndices.Num(), 1);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, TraceFrameAggregate, AllFiniteMatchesSimpleMean, UNTEST_TIMEOUTMS(30000))
{
	// Healthy capture: behaviour must be identical to the pre-fix arithmetic,
	// or the fix has changed every normal reading.
	const TArray<ClaireonTraceFrameStats::FFrameSample> Samples =
		FrameAggregate_MakeSamples({ 10.0, 20.0, 30.0, 40.0 });

	const ClaireonTraceFrameStats::FFrameAggregate Aggregate =
		ClaireonTraceFrameStats::AggregateFrameSamples(Samples);

	UNTEST_EXPECT_EQ(Aggregate.FiniteFrameCount, 4);
	UNTEST_EXPECT_NEAR(Aggregate.AvgMs, 25.0, 0.0001);
	UNTEST_EXPECT_NEAR(Aggregate.MinMs, 10.0, 0.0001);
	UNTEST_EXPECT_NEAR(Aggregate.MaxMs, 40.0, 0.0001);
	UNTEST_EXPECT_EQ(Aggregate.UnterminatedFrameIndices.Num(), 0);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, TraceFrameAggregate, EmptyRangeReportsZeroes, UNTEST_TIMEOUTMS(30000))
{
	const TArray<ClaireonTraceFrameStats::FFrameSample> Samples;

	const ClaireonTraceFrameStats::FFrameAggregate Aggregate =
		ClaireonTraceFrameStats::AggregateFrameSamples(Samples);

	UNTEST_EXPECT_EQ(Aggregate.FiniteFrameCount, 0);
	UNTEST_EXPECT_NEAR(Aggregate.AvgMs, 0.0, 0.0001);
	// Pre-extraction behaviour: min collapses from DBL_MAX to 0 on an empty range.
	UNTEST_EXPECT_NEAR(Aggregate.MinMs, 0.0, 0.0001);
	UNTEST_EXPECT_NEAR(Aggregate.MaxMs, 0.0, 0.0001);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, TraceFrameAggregate, AllUnterminatedReportsEveryIndex, UNTEST_TIMEOUTMS(30000))
{
	// Degenerate but reachable: a capture stopped so early every frame is open.
	// The aggregate must be zeroed rather than infinite, and every index named.
	const TArray<ClaireonTraceFrameStats::FFrameSample> Samples =
		FrameAggregate_MakeSamples({ FrameAggregate_PosInf(), FrameAggregate_PosInf() });

	const ClaireonTraceFrameStats::FFrameAggregate Aggregate =
		ClaireonTraceFrameStats::AggregateFrameSamples(Samples);

	UNTEST_EXPECT_EQ(Aggregate.FiniteFrameCount, 0);
	UNTEST_EXPECT_NEAR(Aggregate.AvgMs, 0.0, 0.0001);
	UNTEST_EXPECT_NEAR(Aggregate.MinMs, 0.0, 0.0001);
	UNTEST_EXPECT_NEAR(Aggregate.MaxMs, 0.0, 0.0001);
	UNTEST_ASSERT_EQ(Aggregate.UnterminatedFrameIndices.Num(), 2);
	UNTEST_EXPECT_EQ(Aggregate.UnterminatedFrameIndices[0], 0);
	UNTEST_EXPECT_EQ(Aggregate.UnterminatedFrameIndices[1], 1);

	co_return;
}

#endif // WITH_UNTESTED
