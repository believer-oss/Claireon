// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that performs full Blueprint comparison: graphs, CDO properties, and SCS components.
 * Supports loading Blueprints from the current editor state or from git revisions.
 */
class ClaireonTool_BlueprintDiff : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// P3: synonym/abbreviation keywords for tools_search ranking
	virtual TArray<FString> GetSearchKeywords() const override;
};
