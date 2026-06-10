// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ListBlueprintGraphNodes.h"
#include "Dom/JsonObject.h"

FString ClaireonTool_ListBlueprintGraphNodes::GetOperation() const { return TEXT("list_nodes"); }

TArray<FString> ClaireonTool_ListBlueprintGraphNodes::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("list"), TEXT("nodes"), TEXT("summary"), TEXT("graph"), TEXT("inspect"), TEXT("outline")};
}

FString ClaireonTool_ListBlueprintGraphNodes::GetDescription() const
{
	return TEXT("List nodes in a Blueprint graph at summary detail. Alias for bp_get_graph(node_detail_level='summary'); exists for discovery -- people reach for list_nodes as a mirror of list_graphs. Stateless / read-only / non-session. Safe during PIE.");
}

TSharedPtr<FJsonObject> ClaireonTool_ListBlueprintGraphNodes::GetInputSchema() const
{
	// Re-use parent schema so list_nodes accepts the full get_graph parameter set
	// (asset_path, graph_name, max_nodes, anchor_node_guid, node_filter, etc.).
	return ClaireonTool_GetBlueprintGraph::GetInputSchema();
}

IClaireonTool::FToolResult ClaireonTool_ListBlueprintGraphNodes::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Copy arguments so we can default node_detail_level to 'summary' without
	// mutating the caller's object. Argument keys that the caller already set are
	// preserved (so an explicit node_detail_level wins; this alias just changes the
	// default).
	TSharedPtr<FJsonObject> Forwarded = Arguments.IsValid()
		? MakeShared<FJsonObject>(*Arguments)
		: MakeShared<FJsonObject>();

	if (!Forwarded->HasField(TEXT("node_detail_level")))
	{
		Forwarded->SetStringField(TEXT("node_detail_level"), TEXT("summary"));
	}

	return ClaireonTool_GetBlueprintGraph::Execute(Forwarded);
}
