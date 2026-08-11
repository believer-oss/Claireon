// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
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
		 * Optional structured next-action nudge. Emitted on every transport that carries a
		 * result: the Python envelope (ClaireonBridge), the MCP wire as a <hint> block
		 * (ClaireonXmlFormatter), and the REPL (ClaireonAnthropicClient).
		 *
		 * Shape: `{tool, reason}` with an optional `args` OR `options` -- never both.
		 *  - `tool` names the tool to call next. SELF-REFERENCE (tool == the tool that just
		 *    ran) is the encoding for "re-issue this call, corrected"; a different name means
		 *    "call that tool instead". No separate discriminator exists, because comparing
		 *    `tool` against what you just called already distinguishes the two.
		 *  - `args`, when present, MUST be a complete, directly-callable argument set -- the
		 *    original call echoed with the correction applied. NEVER a delta: a consumer
		 *    cannot tell a delta from a complete one-argument call, and would re-issue with
		 *    required fields missing.
		 *  - `options` is a candidate set (each entry a complete `args`) meaning "call with
		 *    exactly one of these", for ambiguous-target errors.
		 *
		 * Cardinality: ONE object. Multiple reasons that share a single remedy merge into one
		 * hint with an enumerated `reason` -- two reasons with one action is one hint.
		 *
		 * Two classes with DIFFERENT firing policies -- pick correctly when adding an emitter:
		 *  - Error-derived (the failure itself is the signal): fires per occurrence, carries
		 *    call-specific payload, NOT latched.
		 *  - Success-path guidance (a result was capped, filtered, or omitted by a detail
		 *    level): MUST be latched, keyed by hint code, or it becomes noise on bulk loops --
		 *    the exact complaint that forced a latch onto the get_editor_property nudge.
		 *    `quiet` suppresses without consuming the latch.
		 *
		 * Emitters: ClaireonTool_ExecutePython (error-derived + one script-content channel),
		 * plus the guidance emitters that replaced the former ad-hoc `Data.hint` string
		 * convention (bp_compile_batch, log_search, log_tail). `Data.hint` is retired -- do
		 * not add new ones; this field is the single channel.
		 */
		TSharedPtr<FJsonObject> Hint;

		/** Returns ErrorMessage (if error) or Summary (if success) as a single string. */
		FString GetContentAsString() const
		{
			return bIsError ? ErrorMessage : Summary;
		}

		/** Build a log string from Python log output entries */
		static FString BuildLogString(const TArray<FPythonLogOutputEntry>& LogOutput);
	};

	/**
	 * Deep-copy an argument object so a hint can echo the original call.
	 *
	 * Use this to satisfy the `args` invariant: clone what the caller passed, then apply the
	 * correction (or remove the offending field), so the result stays directly callable.
	 * Returns an empty object when Arguments is invalid.
	 */
	static TSharedPtr<FJsonObject> CloneHintArgs(const TSharedPtr<FJsonObject>& Arguments);

	/**
	 * Build a guidance hint of the form `{tool, reason}` plus an optional complete `args` set.
	 *
	 * CompleteArgs must be a full callable argument set (see CloneHintArgs), not a delta. Pass
	 * nullptr for prose-only guidance, which is a legitimate and already-shipped shape.
	 */
	static TSharedPtr<FJsonObject> MakeGuidanceHint(
		const FString& ToolName,
		const FString& Reason,
		const TSharedPtr<FJsonObject>& CompleteArgs = nullptr);

	/**
	 * Validate hint shape before it reaches a transport.
	 *
	 * Checks that `tool` is present and non-empty and that `args`/`options` are mutually
	 * exclusive. Exists because there were three shapes in the wild with no schema, which is
	 * where drift starts. Returns false and fills OutError when malformed.
	 */
	static bool ValidateHint(const TSharedPtr<FJsonObject>& Hint, FString& OutError);

	/**
	 * Session-scoped latch for SUCCESS-PATH guidance hints, keyed by hint code.
	 *
	 * Returns true the first time a given code is seen this editor session and false after,
	 * so a lesson about a parameter is taught once rather than on every call in a bulk loop.
	 * That noise profile is what forced a latch onto the get_editor_property nudge after three
	 * feedback reports, and a success-path hint has the identical risk.
	 *
	 * Keyed per code (not one bool) because there are several independent lessons; each gets
	 * its own shot. Session-scoped rather than per-asset deliberately: these hints teach a
	 * PARAMETER, and that transfers the moment it is delivered, so a per-asset key would just
	 * lower the repeat count without addressing the complaint. Widening later is a one-line
	 * change to the key; narrowing costs another round of feedback.
	 *
	 * Do NOT use for error-derived hints -- those fire per occurrence, because the failure
	 * itself is the signal and the payload is call-specific.
	 *
	 * CALL ORDER MATTERS: if the tool has a quiet/suppression argument, check it FIRST and
	 * return without calling this, so a suppressed call does not burn a lesson that was never
	 * delivered. Game-thread only.
	 */
	static bool ShouldEmitLatchedHint(FName HintCode);

	/** Clear the latch so tests can exercise second-firing. Mirrors ClaireonPyExec_ResetHintSessionStateForTests. */
	static void ResetHintLatchForTests();

	/**
	 * Execute the tool with the given arguments.
	 * Called on the game thread.
	 * @param Arguments - The parsed arguments from the tools/call request
	 * @return The tool result with structured data and error status
	 */
	/** Whether this tool requires that PIE is NOT running. Default: false (most tools are read-only). */
	virtual bool RequiresNoPIE() const { return false; }

	virtual bool RequiresEditorWorld() const { return false; }

	/** Session-acquisition contract for this tool. Default: ReadOnly. Override on mutating tools. */
	virtual EClaireonToolSessionMode GetSessionMode() const { return EClaireonToolSessionMode::ReadOnly; }

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
	 * MakeSuccessResult plus a hint, as one expression.
	 *
	 * Exists so a hint-carrying return stays a single statement. Hints must ride on
	 * Result.Hint; appending them to Summary corrupts the families whose Summary is
	 * serialized JSON. A null InHint is fine -- the bridge skips it.
	 */
	static FToolResult MakeSuccessResultWithHint(TSharedPtr<FJsonObject> InData, const FString& InSummary, TSharedPtr<FJsonObject> InHint)
	{
		FToolResult Result = MakeSuccessResult(MoveTemp(InData), InSummary);
		Result.Hint = MoveTemp(InHint);
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
