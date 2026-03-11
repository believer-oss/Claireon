// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"
#include "FindInBlueprintManager.h"

/**
 * MCP tool that searches within Blueprint content (nodes, pins, values, comments).
 * Equivalent to the editor's Edit > Find in Blueprints (Ctrl+Shift+F).
 * Uses the engine's FStreamSearch / FFindInBlueprintSearchManager infrastructure.
 */
class ClaireonTool_SearchInBlueprints : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual FString GetCategory() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	/** Convert a filter string (e.g. "nodes", "pins") to ESearchQueryFilter. Returns false if invalid. */
	static bool ParseFilterString(const FString& FilterStr, ESearchQueryFilter& OutFilter);

	/** Recursively format a result tree into indented text lines. */
	static void FormatResultTree(const TSharedPtr<FFindInBlueprintsResult>& Result, int32 Depth, FString& OutText);

	/** Format a single result node with its category and optional comment. */
	static FString FormatResultNode(const TSharedPtr<FFindInBlueprintsResult>& Result);
};
