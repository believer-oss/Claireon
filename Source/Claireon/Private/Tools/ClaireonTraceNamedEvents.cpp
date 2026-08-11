// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonTraceNamedEvents.h"

#include "CoreGlobals.h"

namespace ClaireonTraceNamedEventsInternal
{

// File-local discriminator prefix (TraceNamedEvents_) per module convention.
//
// Depth-counted rather than a bare bool so overlapping captures cannot restore
// the flag while an outer capture is still running -- a leaked forced-on flag
// silently changes the cost profile of every later measurement in the editor
// session, which is the same class of invisible wrongness this item removes.
int32 TraceNamedEvents_PushDepth = 0;
int32 TraceNamedEvents_SavedValue = 0;

} // namespace ClaireonTraceNamedEventsInternal

namespace ClaireonTraceNamedEvents
{

int32 Push()
{
	using namespace ClaireonTraceNamedEventsInternal;

	if (TraceNamedEvents_PushDepth == 0)
	{
		TraceNamedEvents_SavedValue = GCycleStatsShouldEmitNamedEvents;
	}
	++TraceNamedEvents_PushDepth;

	GCycleStatsShouldEmitNamedEvents = 1;
	return GCycleStatsShouldEmitNamedEvents;
}

void Pop()
{
	using namespace ClaireonTraceNamedEventsInternal;

	// An unpaired Pop must not corrupt engine state: without this guard a stop
	// with no matching start would write a stale saved value over whatever is
	// currently in effect.
	if (TraceNamedEvents_PushDepth == 0)
	{
		return;
	}

	--TraceNamedEvents_PushDepth;
	if (TraceNamedEvents_PushDepth == 0)
	{
		GCycleStatsShouldEmitNamedEvents = TraceNamedEvents_SavedValue;
	}
}

bool IsEnabled()
{
	return GCycleStatsShouldEmitNamedEvents > 0;
}

} // namespace ClaireonTraceNamedEvents
