// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP meta-tool that searches the tool registry by query string and category.
 *
 * Query results are returned as a FLAT, globally rank-ordered `tools[]` array
 * (each item: name, category, score, description, signature). Ranking is driven
 * by FClaireonToolSearchIndex (in-memory SQLite FTS5 + bm25), with exact /
 * near-exact name precedence pinning the best name match to result 0. The raw
 * bm25 `score` (negative; lower = better) is emitted per result.
 *
 * Other surfaces: `mode=categories` returns the grouped catalog browse;
 * `select:nameA,nameB` and `name=`/`tool_name=` return deep-inspect records.
 */
class ClaireonTool_SearchTools : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// synonym/abbreviation keywords for search ranking
	virtual TArray<FString> GetSearchKeywords() const override;

#if WITH_UNTESTED
	/**
	 * Test-only seam.  Set Sink to a live TArray<FString>* before calling
	 * Execute(); Execute() will fill it with the full post-sort, pre-truncation
	 * ranked tool names so corpus tests can measure top-K accuracy.
	 * Pass nullptr to disable.  Zero-cost and entirely absent in non-test builds.
	 */
	static void SetExecuteRankedSink(TArray<FString>* Sink);
#endif // WITH_UNTESTED

private:
	/** (Re-)subscribe to OnToolsChanged and rebuild the FTS5 search index
	 *  (FClaireonToolSearchIndex::BuildCatalog) from the current live registry.
	 *  This is the live ranker's catalog build. Returns true on success. */
	bool RebuildSearchIndex();

	/** Number of tools at last catalog rebuild (used for staleness detection) */
	int32 LastCatalogToolCount = 0;
};
