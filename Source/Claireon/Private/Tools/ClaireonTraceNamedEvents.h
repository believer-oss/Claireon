// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"

/**
 * Shared GCycleStatsShouldEmitNamedEvents state for the PIE trace tools.
 *
 * The engine gates every SCOPE_CYCLE_COUNTER-derived trace event on
 * GCycleStatsShouldEmitNamedEvents (Runtime/Core/Public/Stats/Stats.h), while
 * TRACE_CPUPROFILER_EVENT_SCOPE is ungated. A capture taken without forcing
 * the flag on is therefore missing SceneQueryTotal, Physics Tick, World Tick
 * Time and friends -- while still returning hundreds of scopes and looking
 * complete. The absence of a scope is indistinguishable from that scope being
 * cheap, which inverts conclusions.
 *
 * pie_trace_start and pie_trace_stop are separate translation units, so the
 * saved value lives here. Mirrors the project reference implementation in
 * FSPerfTraceDefaults / FSPerfTraceDebugSubsystem: save, force on, restore on
 * stop, roll back on a failed start.
 */
namespace ClaireonTraceNamedEvents
{
/**
 * Saves the current flag value and forces named events on.
 *
 * Idempotent with respect to the saved value: a second Push without an
 * intervening Pop does not clobber what the first one saved.
 *
 * @return the flag value in effect after the call.
 */
int32 Push();

/** Restores the value saved by the outermost Push. No-op if no push is
 *  outstanding, so an unpaired Pop cannot corrupt engine state. */
void Pop();

/** Current flag value, for payload reporting. */
bool IsEnabled();
} // namespace ClaireonTraceNamedEvents
