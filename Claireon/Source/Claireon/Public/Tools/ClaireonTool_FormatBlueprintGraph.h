// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that auto-formats Blueprint graphs using Blueprint Assist or a fallback formatter.
 *
 * This tool provides automatic layout of Blueprint nodes in a graph, organizing them
 * into a readable structure. It prefers Blueprint Assist if available (and enabled),
 * otherwise falls back to a simple topological layout.
 *
 * Input Parameters:
 * - asset_path (string, required): Path to the Blueprint asset (must start with /Game/)
 * - graph_name (string, optional): Name of graph to format (defaults to EventGraph)
 * - formatter (string, optional): "blueprint_assist" or "fallback" (auto-detects if not specified)
 *
 * Output:
 * - Success message with formatter used and node count
 * - Or error if Blueprint/graph not found or formatting failed
 */
class ClaireonTool_FormatBlueprintGraph : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

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

	/**
	 * Format graph using a simple fallback formatter.
	 * This is a basic topological layout that arranges nodes left-to-right.
	 */
	bool FormatWithFallbackFormatter(class UBlueprint* Blueprint, class UEdGraph* Graph, FString& OutError);

	/**
	 * Arrange nodes in a simple left-to-right topological order.
	 * Root nodes (events) on the left, then progressively to the right based on exec flow.
	 */
	void ArrangeNodesTopologically(class UEdGraph* Graph);

	/**
	 * Calculate depth of each node in the execution flow.
	 * Returns a map of node to depth (distance from root nodes).
	 */
	TMap<class UEdGraphNode*, int32> CalculateNodeDepths(class UEdGraph* Graph);

	/**
	 * Perform breadth-first traversal from a root node to calculate depths.
	 */
	void BreadthFirstTraversal(class UEdGraphNode* RootNode, TMap<class UEdGraphNode*, int32>& NodeDepths);
};
