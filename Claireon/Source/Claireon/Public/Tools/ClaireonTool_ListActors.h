// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that lists actors in the currently loaded map.
 * Supports filtering by class name and label wildcard pattern.
 * PIE-aware: when PIE is running, defaults to PIE-world actors.
 */
class ClaireonTool_ListActors : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	// RequiresEditorWorld() is intentionally NOT overridden to true here;
	// the tool handles both editor and PIE worlds internally and performs
	// its own world readiness check in Execute().
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;
};
