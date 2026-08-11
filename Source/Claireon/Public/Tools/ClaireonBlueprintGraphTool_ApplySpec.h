// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonBlueprintGraphEditToolBase.h"

// apply_spec drops the static PIE block. Execute() checks IsPlaySessionInProgress
// at runtime and rejects non-dry-run during PIE; dry_run=true is honored at any time.
// Declared explicitly (not via DECLARE_BPGRAPH_TOOL_PIE_OK) so the search-metadata
// overrides below can exist: sessions repeatedly failed to discover this tool when
// looking for a batch/bulk edit primitive (see Work #6704 round-1 friction report).
class CLAIREON_API ClaireonBlueprintGraphTool_ApplySpec : public ClaireonBlueprintGraphEditToolBase
{
public:
	bool RequiresNoPIE() const override { return false; }
	FString GetOperation() const override;
	FString GetDescription() const override;
	TSharedPtr<FJsonObject> GetInputSchema() const override;
	FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// Search-metadata enrichment: this tool must rank first for batch/bulk
	// edit vocabulary (bp_edit_batch / bp_apply_graph_diff are the names
	// callers invent for it).
	virtual TArray<FString> GetSearchKeywords() const override;
	virtual FString GetPatterns() const override;
	virtual FString GetExampleUsage() const override;
};
