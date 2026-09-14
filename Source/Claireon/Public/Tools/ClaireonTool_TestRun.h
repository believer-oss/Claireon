// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Start an automation run and return immediately; test_poll reads progress and results.
 * Tests require editor frames and must not run in a blocking MCP ticker callback.
 */
class ClaireonTool_TestRun : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};

/**
 * Report the active run for the incoming-MCP gate. Do not use an editor-wide session
 * lock: tests must still open asset sessions and invoke tools directly.
 */
CLAIREON_API bool ClaireonTestRun_IsRunActive(FString& OutRunId);

/** Progress, the final report, or cancellation for the run test_run started. */
class ClaireonTool_TestPoll : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
