// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

// Forward declare CPython types to avoid pulling Python.h into the header
struct _object;
typedef _object PyObject;

class FClaireonServer;

/**
 * C++-to-Python bridge using CPython C API.
 * Registers C++ functions callable from Python via the tools.* namespace.
 * See 04-BRIDGE.md for design details.
 */
class FClaireonBridge
{
public:
	/** Set the server instance used for tool dispatch */
	static void SetToolRegistry(FClaireonServer* Server);

	/** Register all bridge functions with the CPython interpreter */
	static void RegisterBridgeFunctions();

	/** Ensure bridge functions are registered (lazy initialization) */
	static void EnsureRegistered();

	/** Get the result string from the last execute call */
	static FString GetLastExecuteResult();

	/** Reset the last execute result */
	static void ResetLastExecuteResult();

	/** Get the number of tools.* calls made during the last execute */
	static int32 GetToolCallCount();

	/** Reset the tool call counter (called before each execute) */
	static void ResetToolCallCount();

	/**
	 * CPython C function: dispatches tools.* calls to C++ tool registry.
	 * Signature: _mcp_call_tool(tool_name: str, args_json: str) -> str
	 * Public because CPython's PyMethodDef takes a pointer to this at file scope.
	 */
	static PyObject* MCPCallTool(PyObject* Self, PyObject* Args);

	/**
	 * CPython C function: stores result JSON from execute script suffix.
	 * Signature: _mcp_set_result(result_json: str) -> None
	 * Public because CPython's PyMethodDef takes a pointer to this at file scope.
	 */
	static PyObject* MCPSetResult(PyObject* Self, PyObject* Args);

private:
	/** Whether bridge functions have been registered */
	static bool bIsRegistered;

	/** Result from the last execute call */
	static FString GLastExecuteResult;

	/** Pointer to the server instance for tool lookup */
	static FClaireonServer* GServerInstance;

	/** Counter for tools.* calls during a single execute invocation */
	static TAtomic<int32> GToolCallCount;
};
