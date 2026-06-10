// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Tools/IClaireonTool.h"
#include <atomic>

// Forward declare CPython types to avoid pulling Python.h into the header
struct _object;
typedef _object PyObject;

class FClaireonServer;

/** Types of world-transition actions that must be deferred until after Python finishes. */
enum class EClaireonDeferredActionType : uint8
{
	LoadMap,
	PIEStart,
	PIEStop,
	LiveCodingReload,
	DuplicateAndOpenMap,
};

/** A queued world-transition action with optional payload (map path, JSON args, etc.). */
struct FClaireonDeferredAction
{
	EClaireonDeferredActionType Type;
	FString Payload;
};

/** Description of a leaked World package returned by EnsureNoLeakedWorlds. */
struct FClaireonLeakedWorld
{
	/** Long package name of the leaked World, e.g. /Game/Maps/L_MyLevel_Copy. */
	FString PackageName;

	/** True if EnsureNoLeakedWorlds attempted to unload this package. */
	bool bUnloadAttempted = false;

	/** True if the unload attempt succeeded (only meaningful when bUnloadAttempted). */
	bool bUnloadSucceeded = false;

	/** True if the package was dirty and thus skipped (never passed to UnloadPackages). */
	bool bDirty = false;
};

/** Description of an unsaved (dirty) package returned by EnsureNoUnsavedWork. */
struct FClaireonUnsavedPackage
{
	/** Long package name, e.g. /Game/Maps/L_MyLevel or /Game/Items/DA_Sword. */
	FString PackageName;

	/** True if this package contains a map (UPackage::ContainsMap()). */
	bool bIsMapPackage = false;
};

/**
 * C++-to-Python bridge using CPython C API.
 * Registers C++ functions callable from Python via the claireon.* namespace.
 * See 04-BRIDGE.md for design details.
 */
class CLAIREON_API FClaireonBridge
{
public:
	/** Set the server instance used for tool dispatch */
	static void SetToolRegistry(FClaireonServer* Server);

	/** Build the bridge wire envelope JSON object for a tool result.
	 *  Used by both the success and error code paths inside MCPCallTool so the
	 *  on-wire shape is identical (data / hint / summary / warnings / logs /
	 *  ue_log) regardless of success/error.
	 *
	 *  The `hint` field is emitted only when `Result.Hint.IsValid()`; absent
	 *  otherwise so existing wire envelopes stay byte-identical when callers
	 *  do not populate the field. */
	static TSharedPtr<class FJsonObject> BuildResultEnvelope(
		const IClaireonTool::FToolResult& Result);

	/** Register all bridge functions with the CPython interpreter */
	static void RegisterBridgeFunctions();

	/** Ensure bridge functions are registered (lazy initialization) */
	static void EnsureRegistered();

	/** Get the number of claireon.* calls made during the last execute */
	static int32 GetToolCallCount();

	/** Reset the tool call counter (called before each execute) */
	static void ResetToolCallCount();

	/** Set the conversation id propagated to FClaireonOutputGate::RouteResult for spill
	 *  file paths.  Called by the Anthropic REPL client before invoking a tool; pass
	 *  TEXT("default") for direct invocations (diagnostics widget, test harness). */
	static void SetCurrentConversationId(const FString& InConversationId);

	/** Read the current conversation id (for non-REPL call sites like python_execute). */
	static const FString& GetCurrentConversationId();

	/**
	 * CPython C function: dispatches claireon.* calls to C++ tool registry.
	 * Signature: _mcp_call_tool(tool_name: str, args_json: str) -> str
	 * Public because CPython's PyMethodDef takes a pointer to this at file scope.
	 */
	static PyObject* MCPCallTool(PyObject* Self, PyObject* Args);

	/** Enqueue a deferred world-transition action (executed after Python finishes). */
	static void EnqueueDeferredAction(FClaireonDeferredAction Action);

	/** Returns true if any deferred actions are pending. */
	static bool HasDeferredActions();

	/** Returns true if any deferred action would replace or destroy the current world. */
	static bool HasDeferredWorldTransition();

	/** Drain and return all pending deferred actions, clearing the queue. */
	static TArray<FClaireonDeferredAction> DrainDeferredActions();

	/**
	 * Scorched-earth GC barrier: walks every Python object and nulls out
	 * unreal.Object references from all dicts except protected infrastructure
	 * (module dicts, type/class dicts, builtins), then runs gc.collect() x3
	 * and Unreal GC to finalize released wrappers.
	 */
	static void RunWorldTransitionBarrier();

	/**
	 * Find any UWorld objects loaded in memory that are NOT the editor
	 * world, an active PIE/preview world, or a streaming sublevel of the
	 * editor world. Attempt to unload their packages (skipping dirty
	 * ones), run GC, and return any leaks that remain in OutRemaining.
	 *
	 * Returns true iff zero leaks remain (safe to transition).
	 *
	 * Logs every leaked World it touches to LogClaireon regardless of
	 * outcome. Caller is responsible for surfacing the structured error
	 * (see FormatLeakedWorldError + ReportDeferredActionAbort).
	 *
	 * Dirty leaked Worlds are NOT force-unloaded -- they are recorded in
	 * OutRemaining with bDirty=true (see [RESOLVED] D2).
	 */
	static bool EnsureNoLeakedWorlds(TArray<FClaireonLeakedWorld>& OutRemaining);

	/**
	 * Build the human-readable error string for a guard abort. Names
	 * every leaked package, separating dirty (save-or-discard) from
	 * pinned (still-referenced after unload). Single-line if zero
	 * leaks; multi-line otherwise. Prefixed with "[MCP Guard] ..."
	 * style so it is searchable in logs.
	 */
	static FString FormatLeakedWorldError(const TArray<FClaireonLeakedWorld>& Leaks);

	/**
	 * Append a deferred-action abort message to the bridge-side
	 * accumulator. Called from EnsureNoLeakedWorlds callers when the
	 * guard refuses a transition. Multiple aborts in one
	 * python_execute are joined newline-separated by the dispatcher.
	 */
	static void ReportDeferredActionAbort(const FString& Message);

	/**
	 * Scan loaded dirty packages and return those that represent unsaved user
	 * editor work. Filter rule:
	 *   - skip nullptr and GetTransientPackage()
	 *   - skip names beginning with "/Temp/" or "/Memory/" (engine scratch roots)
	 *   - keep everything else, INCLUDING "/Game/", "/Engine/", and third-party
	 *     plugin roots ("/MyPlugin/", etc.) -- plugin assets can be open editor
	 *     work and must be protected.
	 *
	 * Returns true iff zero packages survived the filter (safe to transition).
	 * Caller is responsible for surfacing the structured error via
	 * FormatUnsavedWorkError + ReportDeferredActionAbort.
	 */
	static bool EnsureNoUnsavedWork(TArray<FClaireonUnsavedPackage>& OutDirty);

	/**
	 * Build the human-readable error string for an unsaved-work abort.
	 * Empty input -> empty string (parity with FormatLeakedWorldError).
	 * Non-empty -> single "[MCP Guard] ..." message with comma-separated names
	 * split into "unsaved map(s)" and "unsaved asset(s)" buckets.
	 */
	static FString FormatUnsavedWorkError(const TArray<FClaireonUnsavedPackage>& Dirty);

	/**
	 * Compose the leaked-world guard and the unsaved-work guard into a single
	 * abort decision for the deferred-load-map ticker. OutError receives the
	 * formatted error string when this returns true; empty when false.
	 *
	 * Test-friendly entry point: invoking this exercises the production
	 * composition WITHOUT scheduling a ticker, WITHOUT running
	 * RunWorldTransitionBarrier, and WITHOUT calling FEditorFileUtils::LoadMap.
	 */
	static bool ShouldAbortDeferredLoadMap(FString& OutError);

	/**
	 * Drain and return all reported deferred-action aborts since the
	 * last drain. Empties the accumulator. Called by
	 * ClaireonTool_ExecutePython::Execute after the per-action dispatch
	 * loop.
	 */
	static TArray<FString> DrainDeferredActionAborts();

	/** Rebuild the claireon Python module from the current tool registry. Called when bClaireonModuleStale is set. */
	static void RebuildClaireonModule();

	/** True when the tool registry has changed and the claireon Python module needs rebuilding. */
	static std::atomic<bool> bClaireonModuleStale;

private:
	/** Whether bridge functions have been registered */
	static bool bIsRegistered;

	/** Pointer to the server instance for tool lookup */
	static FClaireonServer* GServerInstance;

	/** Counter for claireon.* calls during a single execute invocation */
	static TAtomic<int32> GToolCallCount;

	/** Queue of deferred world-transition actions */
	static TArray<FClaireonDeferredAction> GDeferredActions;

	/** Stores the OnToolsChanged delegate subscription */
	static FDelegateHandle ToolsChangedHandle;

	/** Build the per-namespace tool catalog and run the Python bootstrap to populate
	 *  sys.modules['<ns>'] for every namespace observed in the registry. */
	static void BuildAndRunBootstrap();

	/** Active conversation id (see SetCurrentConversationId).  Defaults to "default". */
	static FString GCurrentConversationId;

	/** Set of namespaces materialised by the previous bootstrap pass. Used by
	 *  RebuildClaireonModule to delete sys.modules entries for namespaces that
	 *  no longer have any tools (e.g. after a provider unregister). */
	static TSet<FString> PreviousNamespaces;

	/** Accumulator for deferred-action abort messages (drained per python_execute). */
	static TArray<FString> GDeferredActionAborts;
};
