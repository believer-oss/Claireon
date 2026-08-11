// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#pragma once

#include "CoreMinimal.h"

class FJsonObject;

namespace TraceServices
{
class IAnalysisSession;
}

/**
 * Capture manifest (P0-6c).
 *
 * Before this existed, trace_open's entire payload was four fields
 * (session_id, trace_file, duration_ms, frame_count) and no tool anywhere
 * reported which channels a capture was taken with, whether stat named events
 * were on, or whether it contained GPU data at all.
 *
 * That made a capture's suitability unknowable. The specific trap: a capture
 * taken without the gpu channel returns byte-identical results for
 * includeGpu=true and includeGpu=false, and a capture taken without
 * -statnamedevents is missing every SCOPE_CYCLE_COUNTER-derived scope while
 * still returning hundreds of scopes. In both cases the absence of data is
 * indistinguishable from the data being cheap, which inverts conclusions.
 *
 * All three sources read here (channels, diagnostics, timeline event counts)
 * are available on the already-open analysis session at no extra locking cost.
 */
namespace ClaireonTraceCaptureManifest
{
/**
 * Builds the manifest for an open analysis session.
 *
 * MUST be called from inside a TraceServices::FAnalysisSessionReadScope --
 * every provider read below asserts read access.
 *
 * Never returns null: a session missing a provider yields a manifest whose
 * corresponding fields say so, because "we could not tell" and "there is
 * none" are different answers and collapsing them is the defect this fixes.
 */
TSharedPtr<FJsonObject> Build(const TraceServices::IAnalysisSession& Session);
} // namespace ClaireonTraceCaptureManifest
