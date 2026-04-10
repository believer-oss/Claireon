// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

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

	/** Register all bridge functions with the CPython interpreter */
	static void RegisterBridgeFunctions();

	/** Ensure bridge functions are registered (lazy initialization) */
	static void EnsureRegistered();

	/** Get the number of claireon.* calls made during the last execute */
	static int32 GetToolCallCount();

	/** Reset the tool call counter (called before each execute) */
	static void ResetToolCallCount();

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

private:
	/** Whether bridge functions have been registered */
	static bool bIsRegistered;

	/** Pointer to the server instance for tool lookup */
	static FClaireonServer* GServerInstance;

	/** Counter for claireon.* calls during a single execute invocation */
	static TAtomic<int32> GToolCallCount;

	/** Queue of deferred world-transition actions */
	static TArray<FClaireonDeferredAction> GDeferredActions;
};
