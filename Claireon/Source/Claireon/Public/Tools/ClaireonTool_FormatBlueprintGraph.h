// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that auto-formats Blueprint graphs using Blueprint Assist.
 *
 * This tool provides automatic layout of Blueprint nodes in a graph, organizing them
 * into a readable structure. Requires the Blueprint Assist plugin to be installed.
 *
 * Input Parameters:
 * - asset_path (string, required): Path to the Blueprint asset (must start with /Game/)
 * - graph_name (string, optional): Name of graph to format (defaults to EventGraph)
 *
 * Output:
 * - Success message with formatter used and node count
 * - Or error if Blueprint/graph not found or formatting failed
 */
class ClaireonTool_FormatBlueprintGraph : public IClaireonTool
{
public:
	virtual FString GetCategory() const override;
	virtual FString GetOperation() const override;
	virtual bool RequiresNoPIE() const override { return true; }
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	// P3: synonym/abbreviation keywords for search ranking
	virtual TArray<FString> GetSearchKeywords() const override;

private:
	/** Load a Blueprint from an asset path and validate it */
	class UBlueprint* LoadBlueprintFromPath(const FString& AssetPath, FString& OutError);

	/** Find a graph by name in the Blueprint */
	class UEdGraph* FindGraphByName(class UBlueprint* Blueprint, const FString& GraphName);

#if WITH_BLUEPRINT_ASSIST
	/**
	 * Format graph using Blueprint Assist plugin.
	 * Returns true if successful, false if Blueprint Assist is not available or failed.
	 */
	bool FormatWithBlueprintAssist(class UBlueprint* Blueprint, class UEdGraph* Graph, FString& OutError);
#endif
};
