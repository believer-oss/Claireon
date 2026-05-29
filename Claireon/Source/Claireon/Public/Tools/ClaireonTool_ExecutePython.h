// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP Code Mode execution tool.
 * Executes Python code with access to the claireon.* bridge for calling
 * other MCP tools from within Python scripts.
 */
class ClaireonTool_ExecutePython : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::Bypass; }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// P3: synonym/abbreviation keywords for tool_search ranking
	virtual TArray<FString> GetSearchKeywords() const override;

	/**
	 * Install the tool-catalog binding PyCFunctions (_tool_catalog_build,
	 * _tool_catalog_nearest) into the given Python 'unreal' module dict.
	 * Intended to be called once from FClaireonBridge::RegisterBridgeFunctions
	 * under the GIL.  The void* typing avoids leaking Python.h to callers.
	 */
	static void RegisterToolCatalogBindings(void* UnrealModuleDict);

	/**
	 * Parse a captured python_execute log block for one of four signature-class
	 * error patterns and produce a structured hint that tool_search can act on.
	 *
	 * Returns null when no pattern matches, when SyntaxError is detected
	 * (intentionally not nudged), or when a TypeError signature references a
	 * tool name that is not currently registered (avoids hinting on
	 * non-claireon library functions).  Exposed publicly so the test harness
	 * can exercise the parser without spinning up python_execute itself.
	 */
	static TSharedPtr<FJsonObject> BuildHintFromLogs(const FString& Logs);

private:
	/** Maximum allowed script size in bytes */
	static constexpr int32 MaxScriptSizeBytes = 64 * 1024;

	/** Default timeout in milliseconds */
	static constexpr int32 DefaultTimeoutMs = 30000;

	/** Generate the Python prefix code (tools namespace wrapper) */
	static FString GetPythonPrefix();

	/** Generate the Python suffix code (result extraction) */
	static FString GetPythonSuffix();

	/** Monotonic counter for temp file naming */
	static TAtomic<int32> TempFileCounter;
};
