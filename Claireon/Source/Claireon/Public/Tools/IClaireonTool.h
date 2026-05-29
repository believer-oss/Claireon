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

	/** Returns the bare namespace this tool lives under in the Python bridge.
	 *  Default: "claireon". Override to publish under a separate namespace
	 *  (e.g. "fs" for project-private tools). The namespace must be a
	 *  Python-identifier-safe bare token; '.' is disallowed. The bridge
	 *  composes it with GetName() as sys.modules['<namespace>'].<name>.
	 *
	 *  Strict separation contract:
	 *    - GetName() is composed from GetCategory() + "_" + GetOperation();
	 *      both must be bare Python identifiers and the composed result also
	 *      contains no '.'.
	 *    - GetNamespace() must also be a bare identifier (no dot, non-empty).
	 *    - There is no first-dot-split fallback in dispatch. */
	virtual FString GetNamespace() const { return TEXT("claireon"); }

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

		/** Optional structured hint to nudge the agent toward tool_search on
		 *  signature-class failures. Emitted on the wire envelope when valid;
		 *  absent otherwise. See ClaireonTool_ExecutePython for emitters. */
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

	/** Helper to create an error result */
	static FToolResult MakeErrorResult(const FString& InErrorMessage)
	{
		FToolResult Result;
		Result.bIsError = true;
		Result.ErrorMessage = InErrorMessage;
		return Result;
	}
};
