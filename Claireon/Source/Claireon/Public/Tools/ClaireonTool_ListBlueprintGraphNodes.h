// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "Tools/ClaireonTool_GetBlueprintGraph.h"

/**
 * MCP alias tool: `blueprint_list_nodes` -> `blueprint_get_graph(node_detail_level='summary')`.
 *
 * Exists purely to reduce alias-discovery friction. People reach for `list_nodes` as a
 * mirror of `bp_list_graphs`; this tool routes that request to the existing get_graph
 * code path with summary detail. Inherits all parameters from get_graph; sets the
 * default node_detail_level to 'summary' and node_filter (when unspecified) to 'all'.
 *
 * Stateless / read-only / non-session. Safe during PIE.
 */
class ClaireonTool_ListBlueprintGraphNodes : public ClaireonTool_GetBlueprintGraph
{
public:
	virtual FString GetOperation() const override;
	virtual FString GetDescription() const override;
	virtual TSharedPtr<FJsonObject> GetInputSchema() const override;
	virtual FToolResult Execute(const TSharedPtr<FJsonObject>& Arguments) override;

	virtual TArray<FString> GetSearchKeywords() const override;
};
