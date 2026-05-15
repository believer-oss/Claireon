// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"

/**
 * One stream's spill manifest.  Produced by FClaireonOutputGate::RouteResult for each
 * stream whose byte length exceeded the per-stream spill threshold.
 *
 * See CLAIREON_DISK_RESULTS/spill-mechanism.md for the full specification.
 */
struct FClaireonSpillStream
{
	/** Stream identity. Generic tools: "data". claireon.python_execute: "stdout" | "uelog". */
	FString Name;

	/** Absolute, forward-slashed path to the spill file on disk. Empty on write failure. */
	FString AbsolutePath;

	/** Raw stream size in bytes BEFORE any ceiling truncation. */
	int64 SizeBytes = 0;

	/** "application/json" | "text/plain" | "application/octet-stream". */
	FString ContentType;

	/** Up to 1 KiB of UTF-8 text (boundary-safe) or a 256-byte hex dump for binary payloads. */
	FString Preview;

	/** True when the payload hit the per-stream ResultSpillMaxBytes ceiling and was truncated. */
	bool bOverCeiling = false;

	/** True when the temp-then-rename write failed (disk full, permission, locked destination). */
	bool bWriteFailed = false;

	/** OS-provided error text when bWriteFailed is true; empty otherwise. */
	FString ErrorText;
};

/**
 * Aggregated spill manifest for a single FClaireonOutputGate::RouteResult call.  Only
 * streams that actually spilled appear here; under-threshold streams stay inline on
 * the FToolResult.
 */
struct FClaireonSpillResult
{
	/** Zero, one, or two entries depending on tool class and per-stream sizes. */
	TArray<FClaireonSpillStream> Streams;

	bool AnySpilled() const { return Streams.Num() > 0; }
};

/**
 * Identifies which streams a given RouteResult invocation should evaluate.  The gate
 * never infers stream membership from the tool name; the caller explicitly declares
 * the tool class via one of these enumerators.
 *
 * See CLAIREON_DISK_RESULTS/cpp-integration.md "Invocation chokepoints" for the full
 * per-tool-class contract (D6).
 */
enum class EClaireonSpillStreamSet : uint8
{
	/** Single "data" stream.  Used by every generic Claireon tool (asset_search, map_open, ...). */
	GenericData,

	/** Two streams named "stdout" and "uelog".  claireon.python_execute only. */
	PythonStdoutAndUELog,
};

/**
 * Size-based routing for tool results: small payloads stay inline on the wire,
 * large payloads are spilled to disk under <ProjectSavedDir>/Claireon/Results/
 * and the envelope carries a per-stream manifest (path / size / preview).
 *
 * All methods are pure statics; FClaireonOutputGate has no instance state.  All
 * tool execution is game-thread bound, so no locking is required.
 *
 * See CLAIREON_DISK_RESULTS/spill-mechanism.md and cpp-integration.md.
 */
class CLAIREON_API FClaireonOutputGate
{
public:
	/**
	 * Evaluates each stream in StreamSet against UClaireonSettings::ResultSpillThresholdBytes,
	 * spilling over-threshold streams to disk and leaving under-threshold streams inline.
	 *
	 * Returns a (possibly mutated) FToolResult whose envelope carries __mcp_spilled__ : true
	 * and a per-stream spill manifest iff at least one stream spilled.  When spills occur,
	 * the inline blob of each spilled stream is cleared from Result (so the bridge does not
	 * re-serialise it onto the wire).
	 *
	 * Side effects: writes spill files under <ProjectSavedDir>/Claireon/Results/<ConversationId>/,
	 * and calls FClaireonPythonAuditLog::RecordSpill for each spilled (or failed) stream.
	 */
	static IClaireonTool::FToolResult RouteResult(
		IClaireonTool::FToolResult Result,
		const FString& ToolName,
		const FString& ConversationId,
		EClaireonSpillStreamSet StreamSet);

	/**
	 * Connect-time sweep: deletes spill subdirectories under <ProjectSavedDir>/Claireon/Results/
	 * whose most-recent-modified mtime is older than RetentionDays.  Idempotent; callers guard
	 * their own fire-once semantics via a bHasSwept flag.
	 *
	 * Per-file failures (locked handles, antivirus) are tolerated with a Warning log; no throw.
	 */
	static void SweepStaleSpills(int32 RetentionDays);

	/**
	 * Test-only hook: override the <ProjectSavedDir>/Claireon/Results/ root path.  Pass an
	 * empty string to clear the override.  Used by ClaireonOutputGateTests to inject a temp
	 * directory; production code never calls this.
	 */
	static void SetResultsRootOverrideForTests(const FString& InOverridePath);

	/** Returns the currently-effective results root path, taking the test override into account. */
	static FString GetResultsRoot();
};
