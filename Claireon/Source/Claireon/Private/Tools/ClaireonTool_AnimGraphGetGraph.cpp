// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AnimGraphGetGraph.h"
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Animation/AnimBlueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_AnimGraphGetGraph::GetCategory() const { return TEXT("animbp"); }
FString ClaireonTool_AnimGraphGetGraph::GetOperation() const { return TEXT("get_graph"); }

FString ClaireonTool_AnimGraphGetGraph::GetDescription() const
{
	return TEXT("Inspect a specific graph within an Animation Blueprint. Returns all nodes with their types, "
		"categories, pins (with pose connections), property bindings, fast path status, and sub-graph "
		"references. Use animbp_inspect first to discover available graph names.");
}

TSharedPtr<FJsonObject> ClaireonTool_AnimGraphGetGraph::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the Animation Blueprint asset"), true);
	S.AddString(TEXT("graph_name"), TEXT("Name of the specific graph to inspect (from animbp_inspect)"), true);
	S.AddEnum(TEXT("detail_level"), TEXT("'summary' (types/counts), 'nodes' (all pins), 'full' (pins + bindings + fast path)"),
		{TEXT("summary"), TEXT("nodes"), TEXT("full")});
	S.AddInteger(TEXT("max_nodes"), TEXT("Maximum nodes to include (default 100, 0 for unlimited)"));
	S.AddBoolean(TEXT("include_bindings"), TEXT("Include property bindings per node (default true, only at 'full' detail)"));
	S.AddBoolean(TEXT("include_subgraph_summary"), TEXT("Include sub-graph summaries for SM/state/transition nodes (default true)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_AnimGraphGetGraph::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString GraphName;
	if (!Arguments->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: graph_name"));
	}

	FString DetailLevel = TEXT("full");
	Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel);

	int32 MaxNodes = 100;
	if (Arguments->HasField(TEXT("max_nodes")))
	{
		MaxNodes = static_cast<int32>(Arguments->GetNumberField(TEXT("max_nodes")));
	}

	FString Error;
	UAnimBlueprint* AnimBP = ClaireonAnimGraphHelpers::LoadAnimBlueprint(AssetPath, Error);
	if (!AnimBP)
	{
		return MakeErrorResult(Error);
	}

	UEdGraph* Graph = ClaireonAnimGraphHelpers::FindAnimGraphByName(AnimBP, GraphName, Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	// Determine graph type
	FString GraphType = TEXT("Unknown");
	TArray<ClaireonAnimGraphHelpers::FAnimGraphInfo> AllGraphs = ClaireonAnimGraphHelpers::CollectAllGraphs(AnimBP);
	for (const ClaireonAnimGraphHelpers::FAnimGraphInfo& Info : AllGraphs)
	{
		if (Info.Graph == Graph)
		{
			GraphType = Info.Type;
			break;
		}
	}

	// Serialize nodes
	TArray<TSharedPtr<FJsonValue>> NodesArray;
	const int32 MaxNodesToShow = (MaxNodes > 0) ? MaxNodes : Graph->Nodes.Num();
	int32 NodesProcessed = 0;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		if (NodesProcessed >= MaxNodesToShow)
		{
			break;
		}

		TSharedPtr<FJsonObject> NodeObj = ClaireonAnimGraphHelpers::SerializeAnimGraphNode(Node, DetailLevel, AnimBP);
		NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
		NodesProcessed++;
	}

	// Build connections array
	TArray<TSharedPtr<FJsonValue>> ConnectionsArray;
	TSet<UEdGraphNode*> ProcessedSet;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node)
		{
			ProcessedSet.Add(Node);
		}
	}

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != EGPD_Output)
			{
				continue;
			}
			for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (!LinkedPin || !LinkedPin->GetOwningNode())
				{
					continue;
				}
				if (!ProcessedSet.Contains(LinkedPin->GetOwningNode()))
				{
					continue;
				}

				TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
				ConnObj->SetStringField(TEXT("from_node"), Node->NodeGuid.ToString());
				ConnObj->SetStringField(TEXT("from_pin"), Pin->PinName.ToString());
				ConnObj->SetStringField(TEXT("from_pin_type"), Pin->PinType.PinCategory.ToString());
				ConnObj->SetStringField(TEXT("to_node"), LinkedPin->GetOwningNode()->NodeGuid.ToString());
				ConnObj->SetStringField(TEXT("to_pin"), LinkedPin->PinName.ToString());
				ConnectionsArray.Add(MakeShared<FJsonValueObject>(ConnObj));
			}
		}
	}

	// Build result
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("graph_name"), Graph->GetName());
	Data->SetStringField(TEXT("graph_type"), GraphType);
	Data->SetArrayField(TEXT("nodes"), NodesArray);
	Data->SetNumberField(TEXT("node_count"), NodesArray.Num());
	Data->SetNumberField(TEXT("total_nodes_in_graph"), Graph->Nodes.Num());
	Data->SetArrayField(TEXT("connections"), ConnectionsArray);
	Data->SetNumberField(TEXT("connection_count"), ConnectionsArray.Num());

	if (Graph->Nodes.Num() > MaxNodesToShow)
	{
		Data->SetNumberField(TEXT("nodes_not_shown"), Graph->Nodes.Num() - MaxNodesToShow);

		// Build overflow summary by category
		TMap<FString, int32> OverflowCounts;
		int32 Processed = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}
			Processed++;
			if (Processed <= MaxNodesToShow)
			{
				continue;
			}
			FString Category = ClaireonAnimGraphHelpers::GetAnimNodeCategory(Node);
			OverflowCounts.FindOrAdd(Category)++;
		}

		TSharedPtr<FJsonObject> OverflowObj = MakeShared<FJsonObject>();
		for (const auto& Pair : OverflowCounts)
		{
			OverflowObj->SetNumberField(Pair.Key, Pair.Value);
		}
		Data->SetObjectField(TEXT("overflow_by_category"), OverflowObj);
	}

	FString Summary = FString::Printf(TEXT("Graph '%s' (%s): %d/%d nodes, %d connections"),
		*Graph->GetName(), *GraphType, NodesArray.Num(), Graph->Nodes.Num(), ConnectionsArray.Num());

	return MakeSuccessResult(Data, Summary);
}
