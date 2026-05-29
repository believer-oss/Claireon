// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * Advance PIE simulation by N seconds (wall-clock). Companion to
 * wait_seconds; this tool errors when no PIE world is active so callers can
 * compose PIE-only test scenarios without checking the PIE flag separately.
 *
 * Operation aliases: `pie_tick` and `pie_sleep` are exposed as the same logical
 * operation; the only difference is the operator's intent ("advance time" vs
 * "let the world simulate").
 */
class ClaireonTool_PIETick : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};

/** Semantic alias for pie_tick: signals "let the game-thread simulation breathe". */
class ClaireonTool_PIESleep : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
