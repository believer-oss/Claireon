// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/** Kind of advisory captured from an inner tool call. */
enum class EClaireonAdvisoryKind : uint8
{
	Hint,
	Warning,
	Summary,
};

/** How a Data field merges across repeated calls. */
enum class EClaireonAggregationKind : uint8
{
	/** Last-within-target, summed across distinct targets. */
	Sum,
	/** Must match across a target's records; divergence is itself reported. */
	Invariant,
	/** Value -> occurrence count across every record ("6 complete, 14 partial"). */
	Histogram,
	/** Final call wins; rendered only when unambiguous (single target). */
	Last,
	/** Count of distinct values across records ("across 12 targets"). */
	DistinctCount,
};

/** One field of a tool's Tier 1 aggregation spec (see IClaireonTool::GetSummaryAggregationSpec). */
struct FClaireonFieldAggregation
{
	FName Field;
	EClaireonAggregationKind Kind = EClaireonAggregationKind::Last;
};

/**
 * Advisory from an inner claireon.* call, surfaced on the top-level python_execute result.
 * Capture is game-thread only; async tool completions must not append to the accumulator.
 */
struct FClaireonAdvisory
{
	EClaireonAdvisoryKind Kind = EClaireonAdvisoryKind::Summary;

	/** Registry name of the tool that raised it (e.g. "bp_lint"). */
	FString SourceTool;

	/** Hint rate-limit key; NAME_None hints are never rate-limited. */
	FName HintKey;

	/**
	 * Asset path and graph name joined with a colon; session identity is the fallback.
	 * Empty when no target can be resolved.
	 */
	FString Target;

	/** Hint reason / warning text / summary text. */
	FString Text;

	/** Hint only: the full validated hint object. */
	TSharedPtr<FJsonObject> Payload;

	/** Summary Data retained only for tools with a Tier 1 aggregation spec; null otherwise. */
	TSharedPtr<FJsonObject> Data;

	/** Summary only: the source tool's Tier 1 spec, copied at capture so the renderer can
	 *  merge without a tool-instance lookup. Empty = Tier 0. */
	TArray<FClaireonFieldAggregation> AggregationSpec;

	/** How many identical occurrences this record represents after coalescing. */
	int32 OccurrenceCount = 1;
};
