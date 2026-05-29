// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP meta-tool that searches the tool registry by query string and category.
 * Returns matching tools grouped by category with descriptions and schemas.
 *
 * Uses the C++ nearest-string catalog matcher (FClaireonToolCatalogMatcher, BM25-lite
 * with abbreviation / synonym expansion done Python-side by mcp_tool_catalog.py).
 * Falls back to substring matching if Python is unavailable.
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

private:
	/** Rebuild the Python tool catalog index from the current registry */
	bool RebuildCatalog();

	/** Search using the Python catalog; returns ranked tool names or empty on failure.
	 *  Includes GetSearchKeywords() output from each tool as additional match material.
	 *
	 *  Out parameter OutTokensMatchedByName: for each returned tool name,
	 *  the count of distinct query tokens that produced any exact/prefix/fuzzy hit
	 *  for that entry, as reported by FClaireonToolCatalogMatcher::FindNearest. Used
	 *  by Execute() to surface `query_tokens_matched` per result. */
	TArray<FString> FuzzySearch(const FString& Query, int32 MaxResults, TMap<FString, int32>& OutTokensMatchedByName);

	/** Number of tools at last catalog rebuild (used for staleness detection) */
	int32 LastCatalogToolCount = 0;
};
