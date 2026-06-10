// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool: blueprint_compile_batch
 *
 * Compile multiple Blueprints by asset path or content folder. Each entry in
 * paths is auto-detected: a path that resolves to a Blueprint asset is compiled
 * directly; a path that matches a content folder compiles all Blueprints under
 * it recursively. EditorWide session: holds an exclusive lock that blocks all
 * other Claireon sessions for the duration of the batch.
 *
 * Companion to blueprint_compile (single-target, RequiresSession).
 */
class ClaireonTool_BlueprintCompileBatch : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::EditorWide; }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// Synonym/abbreviation keywords for tools_search ranking
	virtual TArray<FString> GetSearchKeywords() const override;
};
