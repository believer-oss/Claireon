// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that probes the Find-in-Blueprints (FiB) cache state.
 *
 * `blueprint_search` blocks on the FiB index, which is built lazily after editor
 * startup. Callers that hit a timeout have no way to ask whether the index is
 * still building or simply slow; this tool exposes the FFindInBlueprintSearchManager
 * cache progress directly so scripts can poll instead of timing out twice.
 *
 * Stateless / read-only / non-session. Safe during PIE.
 */
class ClaireonTool_SearchInBlueprintsIndexStatus : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	virtual TArray<FString> GetSearchKeywords() const override;
};
