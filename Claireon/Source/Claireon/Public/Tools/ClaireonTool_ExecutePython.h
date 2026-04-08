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
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	/** Maximum allowed script size in bytes */
	static constexpr int32 MaxScriptSizeBytes = 64 * 1024;

	/** Default timeout in milliseconds */
	static constexpr int32 DefaultTimeoutMs = 30000;

	/** Generate the Python prefix code (tools namespace wrapper) */
	static FString GetPythonPrefix();

	/** Generate the Python suffix code (result extraction) */
	static FString GetPythonSuffix();

	/** Build concatenated log string from Python command log output */
	static FString BuildLogString(const TArray<struct FPythonLogOutputEntry>& LogOutput);

	/** Monotonic counter for temp file naming */
	static TAtomic<int32> TempFileCounter;
};
