// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"

#include "CoreGlobals.h"
#include "Tools/ClaireonTraceNamedEvents.h"

// ---------------------------------------------------------------------------
// P0-2: pie_trace_start captures were silently missing every stat-derived scope.
//
// The engine gates all SCOPE_CYCLE_COUNTER-derived trace events on
// GCycleStatsShouldEmitNamedEvents (Stats.h:263) while
// TRACE_CPUPROFILER_EVENT_SCOPE is ungated, so a capture taken without forcing
// the flag on has no SceneQueryTotal, Physics Tick or World Tick Time -- while
// still returning hundreds of scopes and looking complete. The absence of a
// scope then reads as that scope being cheap.
//
// ClaireonTool_PIETraceStart::Execute is PIE-gated, so the full path cannot be
// exercised headlessly. These drive the extracted push/pop directly; the
// end-to-end capture check is manual.
//
// Every test restores the global it touches, so ordering against other suites
// cannot be perturbed by a failure here.
// ---------------------------------------------------------------------------
namespace ClaireonTraceNamedEventsTestsInternal
{

// File-local discriminator prefix (NamedEventsTests_) per module convention.

/** RAII restore so a failing assertion cannot leak a forced-on flag into the
 *  rest of the run -- the exact leak the production Pop() exists to prevent. */
struct FNamedEventsTests_ScopedRestore
{
	int32 Saved;
	FNamedEventsTests_ScopedRestore()
		: Saved(GCycleStatsShouldEmitNamedEvents) {}
	~FNamedEventsTests_ScopedRestore() { GCycleStatsShouldEmitNamedEvents = Saved; }
};

} // namespace ClaireonTraceNamedEventsTestsInternal

using namespace ClaireonTraceNamedEventsTestsInternal;

UNTEST_UNIT_OPTS(Claireon, TraceNamedEvents, PushForcesOnAndPopRestoresOff, UNTEST_TIMEOUTMS(30000))
{
	FNamedEventsTests_ScopedRestore Restore;

	GCycleStatsShouldEmitNamedEvents = 0;

	ClaireonTraceNamedEvents::Push();
	UNTEST_EXPECT_TRUE(ClaireonTraceNamedEvents::IsEnabled());
	UNTEST_EXPECT_EQ(GCycleStatsShouldEmitNamedEvents, 1);

	ClaireonTraceNamedEvents::Pop();
	UNTEST_EXPECT_EQ(GCycleStatsShouldEmitNamedEvents, 0);
	UNTEST_EXPECT_FALSE(ClaireonTraceNamedEvents::IsEnabled());

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, TraceNamedEvents, PopRestoresAnAlreadyOnFlag, UNTEST_TIMEOUTMS(30000))
{
	// If the operator already had named events on, stopping a Claireon capture
	// must not turn them off underneath them.
	FNamedEventsTests_ScopedRestore Restore;

	GCycleStatsShouldEmitNamedEvents = 1;

	ClaireonTraceNamedEvents::Push();
	UNTEST_EXPECT_EQ(GCycleStatsShouldEmitNamedEvents, 1);

	ClaireonTraceNamedEvents::Pop();
	UNTEST_EXPECT_EQ(GCycleStatsShouldEmitNamedEvents, 1);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, TraceNamedEvents, DoublePushDoesNotClobberSavedValue, UNTEST_TIMEOUTMS(30000))
{
	// Overlapping captures must not restore the flag while an outer capture is
	// still recording. Only the outermost Pop restores.
	FNamedEventsTests_ScopedRestore Restore;

	GCycleStatsShouldEmitNamedEvents = 0;

	ClaireonTraceNamedEvents::Push();
	ClaireonTraceNamedEvents::Push();
	UNTEST_EXPECT_EQ(GCycleStatsShouldEmitNamedEvents, 1);

	ClaireonTraceNamedEvents::Pop();
	UNTEST_EXPECT_EQ(GCycleStatsShouldEmitNamedEvents, 1);

	ClaireonTraceNamedEvents::Pop();
	UNTEST_EXPECT_EQ(GCycleStatsShouldEmitNamedEvents, 0);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, TraceNamedEvents, UnpairedPopIsANoOp, UNTEST_TIMEOUTMS(30000))
{
	// A stop with no matching start must not write a stale saved value over
	// whatever is currently in effect.
	FNamedEventsTests_ScopedRestore Restore;

	GCycleStatsShouldEmitNamedEvents = 1;

	ClaireonTraceNamedEvents::Pop();
	UNTEST_EXPECT_EQ(GCycleStatsShouldEmitNamedEvents, 1);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, TraceNamedEvents, FailedStartRollsBack, UNTEST_TIMEOUTMS(30000))
{
	// Models the !bStarted branch in ClaireonTool_PIETraceStart: a failed start
	// must not leave the flag forced on for the rest of the editor session,
	// silently changing the cost profile of every later measurement.
	FNamedEventsTests_ScopedRestore Restore;

	GCycleStatsShouldEmitNamedEvents = 0;

	ClaireonTraceNamedEvents::Push();
	ClaireonTraceNamedEvents::Pop(); // the rollback the failure branch performs

	UNTEST_EXPECT_EQ(GCycleStatsShouldEmitNamedEvents, 0);
	UNTEST_EXPECT_FALSE(ClaireonTraceNamedEvents::IsEnabled());

	co_return;
}

#endif // WITH_UNTESTED
