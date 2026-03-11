// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PCGGraphInspect.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "ClaireonLog.h"
#include "Misc/Paths.h"
#include "PCGGraph.h"
#include "PCGNode.h"

FString ClaireonTool_PCGGraphInspect::GetName() const
{
	return TEXT("inspect_pcg_graph");
}

FString ClaireonTool_PCGGraphInspect::GetCategory() const
{
	return TEXT("pcg");
}

FString ClaireonTool_PCGGraphInspect::GetDescription() const
{
	return TEXT("Read the structure of a PCG (Procedural Content Generation) graph asset as structured text. "
				"Displays nodes, pins, connections, and settings properties. "
				"Use detail_level='summary' for a compact overview, 'full' for complete property details, or 'outline' for node names only. "
				"Optionally inspect a single node by providing node_id (index, name, or 'input'/'output').");
}

TSharedPtr<FJsonObject> ClaireonTool_PCGGraphInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path to the PCG Graph (e.g. /Game/PCG/PCG_ForestSpawner)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// detail_level - optional
	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"), TEXT("Level of detail: 'summary' (pins + connections), 'full' (+ properties), 'outline' (node names only). Default: summary"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("outline")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("full")));
		DetailProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("detail_level"), DetailProp);

	// node_id - optional
	TSharedPtr<FJsonObject> NodeIdProp = MakeShared<FJsonObject>();
	NodeIdProp->SetStringField(TEXT("type"), TEXT("string"));
	NodeIdProp->SetStringField(TEXT("description"), TEXT("Optional node identifier (numeric index, node title, or 'input'/'output') to inspect a single node in detail"));
	Properties->SetObjectField(TEXT("node_id"), NodeIdProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PCGGraphInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse required parameter
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	// Parse optional parameters
	FString DetailLevel = TEXT("summary");
	Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel);

	FString NodeIdStr;
	Arguments->TryGetStringField(TEXT("node_id"), NodeIdStr);

	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.pcg.inspect: asset=%s, detail=%s, node_id=%s"),
		*AssetPath, *DetailLevel, *NodeIdStr);

	// Load asset
	FString Error;
	UPCGGraph* Graph = ClaireonPCGGraphHelpers::LoadPCGGraphAsset(AssetPath, Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	// If node_id specified, inspect just that node
	if (!NodeIdStr.IsEmpty())
	{
		int32 NodeIndex;
		UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Graph, NodeIdStr, NodeIndex);
		if (!Node)
		{
			return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeIdStr));
		}

		FString NodeDetail = ClaireonPCGGraphHelpers::FormatNodeDetail(Graph, Node, NodeIndex, true);

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("asset_path"), AssetPath);
		Data->SetStringField(TEXT("node_detail"), NodeDetail);

		return MakeSuccessResult(Data, FString::Printf(TEXT("Node [%d] %s"),
			NodeIndex, *ClaireonPCGGraphHelpers::GetNodeDisplayName(Node)));
	}

	// Full graph inspection
	const TArray<UPCGNode*>& Nodes = Graph->GetNodes();
	FString GraphStructure = ClaireonPCGGraphHelpers::FormatGraphStructure(Graph, DetailLevel);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetNumberField(TEXT("node_count"), Nodes.Num());
	Data->SetStringField(TEXT("graph_structure"), GraphStructure);

	FString AssetName = FPaths::GetBaseFilename(AssetPath);
	return MakeSuccessResult(Data, FString::Printf(TEXT("%s: %d nodes"), *AssetName, Nodes.Num()));
}
