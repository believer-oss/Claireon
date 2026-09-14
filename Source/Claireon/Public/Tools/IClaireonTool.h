// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "ClaireonAdvisory.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

struct FPythonLogOutputEntry;

/**
 * Per-tool session-acquisition contract enforced by the Claireon bridge.
 *
 * - ReadOnly: no FClaireonSessionManager interaction; bridge forwards unconditionally.
 * - RequiresSession: tool (or its RAII helper) calls OpenSession on the asset(s)
 *   it mutates and surfaces BlockedByOtherTool on contention.
 * - Bypass: bridge consults FClaireonSessionManager::ListSessions() first and
 *   refuses if any session is held by a different tool. Carve-outs:
 *   session_release and session_list are always allowed.
 * - EditorWide: bridge calls OpenEditorWideSession before forwarding and
 *   CloseEditorWideSession after (RAII at the bridge layer).
 *
 * Static per descriptor. If a tool needs both single-target (RequiresSession)
 * and batch (EditorWide) shapes, split into two descriptors.
 */
enum class EClaireonToolSessionMode : uint8
{
	ReadOnly,
	RequiresSession,
	Bypass,
	EditorWide
};

/**
 * Interface for MCP tools that can be registered with the server.
 * Each tool provides a name, description, input schema, and an Execute method.
 */
class CLAIREON_API IClaireonTool
{
public:
	virtual ~IClaireonTool() = default;

	/** Returns the tool's category/domain (e.g. "gas", "audio", "blueprint").
	 *  Authoritative -- composed with GetOperation() to produce GetName().
	 *  Must be a bare Python identifier (no dot, non-empty). */
	virtual FString GetCategory() const = 0;

	/** Returns the verb/operation portion of the tool name (e.g. "set_property",
	 *  "inspect"). Composed with GetCategory() to produce GetName().
	 *  Must be a bare Python identifier (no dot, non-empty). */
	virtual FString GetOperation() const = 0;

	/** Composed wire name: `<category>_<operation>`. Sealed -- a tool's name
	 *  is derived from GetCategory() + GetOperation() so the two cannot
	 *  disagree by construction. To rename a tool, change GetCategory() or
	 *  GetOperation(); never override GetName() in a subclass. */
	virtual FString GetName() const final
	{
		return GetCategory() + TEXT("_") + GetOperation();
	}

	/** Returns a human-readable description of what the tool does (standard tier, ~150-300 chars) */
	virtual FString GetDescription() const = 0;

	/** One-line summary (~40-80 chars). Default: first sentence of GetDescription(). */
	virtual FString GetBriefDescription() const
	{
		FString Desc = GetDescription();
		// Extract first sentence (up to ". " or first 100 chars)
		int32 DotPos;
		if (Desc.FindChar(TEXT('.'), DotPos) && DotPos < 100)
		{
			return Desc.Left(DotPos + 1);
		}
		if (Desc.Len() <= 100)
		{
			return Desc;
		}
		return Desc.Left(97) + TEXT("...");
	}

	/** Extended docs with examples, workflows, recovery procedures. Default: same as GetDescription(). */
	virtual FString GetFullDescription() const { return GetDescription(); }

	/** Returns the JSON Schema for the tool's input parameters */
	virtual TSharedPtr<FJsonObject> GetInputSchema() const = 0;

	/** Result of a tool execution */
	struct FToolResult
	{
		/** Structured result data (primary), serialized to Python dict */
		TSharedPtr<FJsonObject> Data;

		/** Tool-generated summary for XML <summary> tag */
		FString Summary;

		/** Non-fatal warnings */
		TArray<FString> Warnings;

		/** Execution logs (stdout/stderr lines) */
		FString Logs;

		/** Engine UE_LOG output captured during execution (Warning/Error level) */
		FString UELog;

		/** Whether this result represents an error */
		bool bIsError = false;

		/** Error description */
		FString ErrorMessage;

		/**
		 * Validated next-action hints. The Python envelope emits hints plus a legacy first-entry
		 * hint alias; MCP emits repeated <hint> blocks. Invalid entries are dropped individually.
		 *
		 * Emit at each tool call. Request-scoped rate limiting and coalescing handle repetition;
		 * use a stable key for guidance and leave error-derived hints keyless. See ValidateHint
		 * for the schema. Keep hints out of Data and Summary.
		 */
		TArray<TSharedPtr<FJsonObject>> Hints;

		/** Append a hint; null is skipped so call sites can pass possibly-null builders. */
		void AddHint(TSharedPtr<FJsonObject> InHint)
		{
			if (InHint.IsValid())
			{
				Hints.Add(MoveTemp(InHint));
			}
		}

		/**
		 * Nested-call advisories collected by python_execute. Warnings and hints reach the
		 * MCP formatter; summary records become the summary rollup. Omit these from the
		 * Python result envelope so script data handling cannot swallow them. Nested capture
		 * propagates them directly, outside the output spill gate.
		 */
		TArray<FClaireonAdvisory> InnerAdvisories;

		/** Returns ErrorMessage (if error) or Summary (if success) as a single string. */
		FString GetContentAsString() const
		{
			return bIsError ? ErrorMessage : Summary;
		}

		/** Build a log string from Python log output entries */
		static FString BuildLogString(const TArray<FPythonLogOutputEntry>& LogOutput);
	};

	/** Deep-copy arguments before applying a hint correction. Invalid input yields an empty object. */
	static TSharedPtr<FJsonObject> CloneHintArgs(const TSharedPtr<FJsonObject>& Arguments);

	/**
	 * Build {tool, reason} with optional complete callable arguments, never a delta.
	 * Pass nullptr when no concrete argument set is available.
	 */
	static TSharedPtr<FJsonObject> MakeGuidanceHint(
		const FString& ToolName,
		const FString& Reason,
		const TSharedPtr<FJsonObject>& CompleteArgs = nullptr,
		FName Key = NAME_None);

	/**
	 * Build {resource, reason} with a scheme-qualified MCP URI. Resource hints take
	 * neither args nor options.
	 */
	static TSharedPtr<FJsonObject> MakeResourceHint(
		const FString& ResourceUri,
		const FString& Reason,
		FName Key = NAME_None);

	/**
	 * Validate hint shape; return false with OutError on malformed input.
	 * Exactly one non-empty tool or resource is required, plus a non-empty reason.
	 * Resources must be scheme-qualified URIs. Tool hints may carry either a complete
	 * args object or options containing alternative complete argument objects.
	 * An optional non-empty key identifies rate-limited guidance. Reject unknown keys.
	 */
	static bool ValidateHint(const TSharedPtr<FJsonObject>& Hint, FString& OutError);


	/** Whether this tool requires that PIE is NOT running. Default: false (most tools are read-only). */
	virtual bool RequiresNoPIE() const { return false; }

	virtual bool RequiresEditorWorld() const { return false; }

	/** Session-acquisition contract for this tool. Default: ReadOnly. Override on mutating tools. */
	virtual EClaireonToolSessionMode GetSessionMode() const { return EClaireonToolSessionMode::ReadOnly; }

	/**
	 * Execute the tool with the given arguments.
	 * Called on the game thread.
	 * @param Arguments - The parsed arguments from the tools/call request
	 * @return The tool result with structured data and error status
	 */
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) = 0;

	/** Optional example usage string shown in deep-inspect / search output. Default: empty. */
	virtual FString GetExampleUsage() const { return FString(); }

	/** Optional patterns / common-pitfalls / see-also block. Returned as markdown.
	 *  Empty string suppresses surfacing entirely in tool_search responses. */
	virtual FString GetPatterns() const { return FString(); }

	/** Optional per-parameter tooltip map (parameter name -> tooltip string). Default: null. */
	virtual TSharedPtr<FJsonObject> GetParameterTooltips() const { return nullptr; }

	/** Optional search-keyword list used to boost fuzzy-search matches for this tool. Default: empty. */
	virtual TArray<FString> GetSearchKeywords() const { return {}; }

	/**
	 * Opt into field-specific summary aggregation. Returning true also makes the bridge
	 * capture Data for summary advisories; the default uses unstructured summaries.
	 */
	virtual bool GetSummaryAggregationSpec(TArray<FClaireonFieldAggregation>& OutSpec) const
	{
		return false;
	}

	/** Helper to create a success result with structured data and summary */
	static FToolResult MakeSuccessResult(TSharedPtr<FJsonObject> InData, const FString& InSummary)
	{
		FToolResult Result;
		Result.Data = MoveTemp(InData);
		Result.Summary = InSummary;
		Result.bIsError = false;
		return Result;
	}

	/**
	 * Create success with an optional hint. Keep hints separate from Summary, which
	 * some families serialize as JSON. Null hints are skipped.
	 */
	static FToolResult MakeSuccessResultWithHint(TSharedPtr<FJsonObject> InData, const FString& InSummary, TSharedPtr<FJsonObject> InHint)
	{
		FToolResult Result = MakeSuccessResult(MoveTemp(InData), InSummary);
		Result.AddHint(MoveTemp(InHint));
		return Result;
	}

	/** Helper to create an error result */
	static FToolResult MakeErrorResult(const FString& InErrorMessage)
	{
		FToolResult Result;
		Result.bIsError = true;
		Result.ErrorMessage = InErrorMessage;
		return Result;
	}
};
