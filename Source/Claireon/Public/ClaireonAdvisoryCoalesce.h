// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "ClaireonAdvisory.h"

/** Collapse duplicate advisories within one python_execute invocation, preserving occurrence counts. */
namespace ClaireonAdvisoryCoalesce
{
	/**
	 * Collapse duplicates in first-occurrence order. Identity per kind:
	 *   Hint: (SourceTool, HintKey, Target, Text)
	 *   Warning: (SourceTool, Target, Text)
	 *   Summary: (SourceTool, Target); last text/payload wins, with the total call count.
	 */
	CLAIREON_API TArray<FClaireonAdvisory> Coalesce(const TArray<FClaireonAdvisory>& Raw);

	/**
	 * Join non-empty asset_path and graph_name with a colon, using session:<id> if asset_path
	 * is absent. Return empty when no target fields exist or Arguments is invalid.
	 */
	CLAIREON_API FString ExtractTarget(const TSharedPtr<FJsonObject>& Arguments);

	/**
	 * Render coalesced summaries without aggregation specs, with optional counts and targets.
	 * Rows beyond the byte budget are replaced by an elision count. The default leaves room
	 * for python_execute's own summary and output-gate markers in the 2048-byte window.
	 */
	CLAIREON_API FString RenderSummaryRollup(
		const TArray<FClaireonAdvisory>& Coalesced,
		int32 MaxBytes = 1400);

	/**
	 * Render one merged line per tool with an aggregation spec, reporting invariant divergence.
	 * Pass raw records: histograms and invariants require every call. Records without specs
	 * are handled by RenderSummaryRollup.
	 */
	CLAIREON_API FString RenderTier1Rollup(
		const TArray<FClaireonAdvisory>& Raw,
		int32 MaxBytes = 1400);

	/**
	 * UTF-8 bytes in source tools, targets, texts, and serialized hint payloads.
	 * Used by the attach seam's 4096-byte emitter assertion; does not truncate advisories.
	 */
	CLAIREON_API int32 MeasureWireAdvisoryBytes(const TArray<FClaireonAdvisory>& Advisories);
}
