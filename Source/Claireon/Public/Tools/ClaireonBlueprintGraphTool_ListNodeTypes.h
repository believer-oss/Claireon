// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

/**
 * Enumerate every node_type bp_add_node accepts, with its required and optional
 * parameters, as structured data.
 *
 * Read-only and session-free: it describes the tool surface, not any asset.
 */
class CLAIREON_API ClaireonBlueprintGraphTool_ListNodeTypes : public ClaireonBlueprintGraphEditToolBase
{
public:
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	/** Describes the tool surface, so it needs neither a session nor a PIE guard. */
	virtual EClaireonToolSessionMode GetSessionMode() const override { return EClaireonToolSessionMode::ReadOnly; }
	virtual bool RequiresNoPIE() const override { return false; }

	// hot-path metadata enrichment
	virtual FString GetFullDescription() const override;
	virtual FString GetExampleUsage() const override;

	// synonym/abbreviation keywords for search ranking
	virtual TArray<FString> GetSearchKeywords() const override;
};
