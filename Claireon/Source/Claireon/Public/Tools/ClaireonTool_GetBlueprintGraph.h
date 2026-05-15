// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/IClaireonTool.h"

/**
 * MCP tool that exports a Blueprint graph's structure for AI inspection.
 *
 * Outputs graph as:
 * - JSON summary (nodes, pins, connections) with configurable detail levels
 * - T3D text format (for round-trip editing)
 * - Both formats combined
 *
 * Supports detail levels:
 * - "full": Complete node/pin information
 * - "summary": Core information without defaults
 * - "outline": Just node types and connections
 */
class ClaireonTool_GetBlueprintGraph : public IClaireonTool
{
public:
	virtual FString GetName() const override;
	virtual FString GetDescription() const override;
	virtual FString GetFullDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

private:
	/** Load a Blueprint from an asset path and validate it */
	class UBlueprint* LoadBlueprintFromPath(const FString& AssetPath, FString& OutError);

	/** Find a graph by name within a Blueprint */
	class UEdGraph* FindGraphByName(const class UBlueprint* Blueprint, const FString& GraphName, FString& OutError);

	/** Build JSON summary of graph structure */
	FString BuildGraphJsonSummary(const class UEdGraph* Graph, const FString& DetailLevel, int32 MaxNodes, const FString& AnchorGuid = FString(), int32 TraversalDepth = -1);

	/** Build T3D export of graph nodes */
	FString BuildGraphT3DExport(const class UEdGraph* Graph);

	/** Format a single node's summary based on detail level */
	FString FormatNodeSummary(const class UEdGraphNode* Node, const FString& DetailLevel);

	/** Format pin information (connections, defaults, type) */
	FString FormatPinInfo(const class UEdGraphPin* Pin, const FString& DetailLevel);

	/** Build overflow summary when max_nodes limit is reached */
	FString BuildOverflowSummary(const class UEdGraph* Graph, int32 NumShown, int32 TotalNodes);

	/** Get a pin's type as a formatted string */
	static FString GetPinTypeString(const class UEdGraphPin* Pin);

	/** Get a node's title/display name */
	static FString GetNodeTitle(const class UEdGraphNode* Node);

	/** Check if detail level is valid */
	static bool IsValidDetailLevel(const FString& DetailLevel);

	// Test access: allow spec tests to call private formatters directly so they can
	// verify the outline grammar / regex without needing a markdown wrapper in Execute.
	friend struct FClaireonGetBlueprintGraphTestAccess;
};

/** Test-only accessor exposing private formatters for automation tests. */
struct FClaireonGetBlueprintGraphTestAccess
{
	static FString FormatNodeSummary(ClaireonTool_GetBlueprintGraph& Tool, const class UEdGraphNode* Node, const FString& DetailLevel)
	{
		return Tool.FormatNodeSummary(Node, DetailLevel);
	}
	static FString BuildGraphJsonSummary(ClaireonTool_GetBlueprintGraph& Tool, const class UEdGraph* Graph, const FString& DetailLevel, int32 MaxNodes)
	{
		return Tool.BuildGraphJsonSummary(Graph, DetailLevel, MaxNodes);
	}
};
