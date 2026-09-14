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
 * Report capture capabilities so missing channels or named events are not mistaken
 * for low cost. Read channels, diagnostics, and timeline counts from the open session.
 */
namespace ClaireonTraceCaptureManifest
{
/**
 * Build under FAnalysisSessionReadScope; provider reads require it. Never returns
 * null. Missing providers are reported as unavailable, not empty.
 */
TSharedPtr<FJsonObject> Build(const TraceServices::IAnalysisSession& Session);
} // namespace ClaireonTraceCaptureManifest
