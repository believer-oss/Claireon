// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GetBlueprintGraph.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLog.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "EdGraphUtilities.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraphSchema.h"
#include "WidgetBlueprint.h"

FString ClaireonTool_GetBlueprintGraph::GetName() const
{
	return TEXT("get_blueprint_graph");
}

FString ClaireonTool_GetBlueprintGraph::GetCategory() const
{
	return TEXT("blueprint");
}

FString ClaireonTool_GetBlueprintGraph::GetDescription() const
{
	return TEXT("Read Blueprint graph structure at configurable detail levels (exec/full/summary/outline). Supports BFS traversal from anchor nodes and JSON or T3D export formats.");
}

FString ClaireonTool_GetBlueprintGraph::GetFullDescription() const
{
	return TEXT("Read the internal graph structure of a Blueprint.\n"
				"Default output: JSON summary at 'exec' detail level â node titles, classes, GUIDs, positions, exec-pin connections, and compact data-pin counts. Suitable for surveying large graphs without token overflow.\n\n"
				"Parameters:\n"
				"  asset_path (required): Unreal content path of the Blueprint (e.g., /Game/Characters/BP_PlayerCharacter).\n"
				"  graph_name (optional): Name of a specific graph to export (e.g., 'EventGraph', 'TakeDamage'). Omit to export all graphs.\n"
				"  format (optional, default='json'): 'json' for structured summary, 't3d' for Unreal text export (clipboard copy/paste), 'both' for both. T3D is opt-in.\n"
				"  node_detail_level (optional, default='exec'):\n"
				"    'exec' â compact view: title, class, GUID, position, exec-pin connections, and compact data-pin count. Best for large graphs.\n"
				"    'full' â all pin details and defaults.\n"
				"    'summary' â node types and connections only (no pin defaults).\n"
				"    'outline' â node names only.\n"
				"  include_pin_defaults (optional): Include default values for unconnected pins. Ignored in 'exec' mode. In 'summary' mode defaults to false.\n"
				"  max_nodes (optional, default=100): Maximum nodes to include. Use 0 for unlimited. Capped at 50 when anchor_node_guid is used.\n"
				"  anchor_node_guid (optional): GUID of a node to anchor BFS traversal. When provided, returns only nodes reachable via exec connections from this node (up to 50). "
				"Use node_detail_level='exec' on the full graph first to get GUIDs, then anchor + node_detail_level='full' to drill into a specific section.\n"
				"  traversal_depth (optional, default=-1): BFS hop limit when anchor_node_guid is set. 0=anchor only, 1=anchor+direct neighbors, 2=two hops, -1=unlimited.\n\n"
				"Navigation workflow: Call with defaults to get a compact exec-level overview and node GUIDs. Then use anchor_node_guid=<guid> with node_detail_level='full' to inspect a specific subgraph.\n"
				"Use editor.blueprint.getProperties first to discover available graphs.");
}

TSharedPtr<FJsonObject> ClaireonTool_GetBlueprintGraph::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
	PathProp->SetStringField(TEXT("type"), TEXT("string"));
	PathProp->SetStringField(TEXT("description"), TEXT("Unreal content path of the Blueprint asset (e.g., /Game/Characters/BP_PlayerCharacter). Must start with /Game/."));
	Properties->SetObjectField(TEXT("asset_path"), PathProp);

	// graph_name - optional
	TSharedPtr<FJsonObject> GraphProp = MakeShared<FJsonObject>();
	GraphProp->SetStringField(TEXT("type"), TEXT("string"));
	GraphProp->SetStringField(TEXT("description"), TEXT("Name of the specific graph to export (e.g., 'EventGraph', 'TakeDamage'). If omitted, exports all graphs."));
	Properties->SetObjectField(TEXT("graph_name"), GraphProp);

	// format - optional
	TSharedPtr<FJsonObject> FormatProp = MakeShared<FJsonObject>();
	FormatProp->SetStringField(TEXT("type"), TEXT("string"));
	TArray<TSharedPtr<FJsonValue>> FormatEnum;
	FormatEnum.Add(MakeShared<FJsonValueString>(TEXT("both")));
	FormatEnum.Add(MakeShared<FJsonValueString>(TEXT("json")));
	FormatEnum.Add(MakeShared<FJsonValueString>(TEXT("t3d")));
	FormatProp->SetArrayField(TEXT("enum"), FormatEnum);
	FormatProp->SetStringField(TEXT("description"), TEXT("Output format: 'json' for structured summary (default), 't3d' for Unreal text export, 'both' for both (includes T3D, opt-in for clipboard copy/paste use cases). Default: 'json'."));
	Properties->SetObjectField(TEXT("format"), FormatProp);

	// include_pin_defaults - optional
	TSharedPtr<FJsonObject> DefaultsProp = MakeShared<FJsonObject>();
	DefaultsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	DefaultsProp->SetStringField(TEXT("description"), TEXT("Include default values for unconnected pins in JSON output. Default: true. In 'exec' node_detail_level mode, this parameter is ignored (no data pins shown). In 'summary' mode, defaults to false."));
	Properties->SetObjectField(TEXT("include_pin_defaults"), DefaultsProp);

	// node_detail_level - optional
	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	TArray<TSharedPtr<FJsonValue>> DetailEnum;
	DetailEnum.Add(MakeShared<FJsonValueString>(TEXT("exec")));
	DetailEnum.Add(MakeShared<FJsonValueString>(TEXT("full")));
	DetailEnum.Add(MakeShared<FJsonValueString>(TEXT("summary")));
	DetailEnum.Add(MakeShared<FJsonValueString>(TEXT("outline")));
	DetailProp->SetArrayField(TEXT("enum"), DetailEnum);
	DetailProp->SetStringField(TEXT("description"), TEXT("Level of detail for node output. Default: 'exec'.\n"
														 "'exec' â compact view: node title, class, GUID, position, exec-pin connections only, and a compact data pin count (N data pins). Best for surveying large graphs.\n"
														 "'full' â all pin details and defaults.\n"
														 "'summary' â node types and connections only (no pin defaults).\n"
														 "'outline' â node names only."));
	Properties->SetObjectField(TEXT("node_detail_level"), DetailProp);

	// max_nodes - optional
	TSharedPtr<FJsonObject> MaxProp = MakeShared<FJsonObject>();
	MaxProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxProp->SetStringField(TEXT("description"), TEXT("Maximum number of nodes to include in full detail. Remaining nodes are shown as a summary count. Default: 100. Use 0 for unlimited."));
	Properties->SetObjectField(TEXT("max_nodes"), MaxProp);

	// anchor_node_guid - optional
	TSharedPtr<FJsonObject> AnchorProp = MakeShared<FJsonObject>();
	AnchorProp->SetStringField(TEXT("type"), TEXT("string"));
	AnchorProp->SetStringField(TEXT("description"), TEXT("When provided, start node listing from this GUID using BFS along exec connections. "
														 "Nodes are ordered by BFS discovery (exec flow order). Max nodes capped at 50 when anchor is used. "
														 "Use node_detail_level=exec on the full graph first to get GUIDs, then anchor + node_detail_level=full to drill into a specific area."));
	Properties->SetObjectField(TEXT("anchor_node_guid"), AnchorProp);

	// traversal_depth - optional
	TSharedPtr<FJsonObject> TraversalDepthProp = MakeShared<FJsonObject>();
	TraversalDepthProp->SetStringField(TEXT("type"), TEXT("integer"));
	TraversalDepthProp->SetStringField(TEXT("description"), TEXT("BFS depth limit when anchor_node_guid is provided. "
																 "0=anchor only, 1=anchor + direct exec neighbors, 2=two hops, -1=unlimited (subject to max_nodes cap). "
																 "Ignored when anchor_node_guid is not provided."));
	Properties->SetObjectField(TEXT("traversal_depth"), TraversalDepthProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_GetBlueprintGraph::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Validate asset_path
	if (!Arguments->HasField(TEXT("asset_path")))
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString AssetPath = Arguments->GetStringField(TEXT("asset_path"));
	FString ValidationError;
	if (!ValidateAssetPath(AssetPath, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	// Optional parameters
	FString GraphName;
	if (Arguments->HasField(TEXT("graph_name")))
	{
		GraphName = Arguments->GetStringField(TEXT("graph_name"));
	}

	FString DetailLevel = TEXT("exec");
	if (Arguments->HasField(TEXT("node_detail_level")))
	{
		DetailLevel = Arguments->GetStringField(TEXT("node_detail_level"));
		if (!IsValidDetailLevel(DetailLevel))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid node_detail_level: %s. Must be one of: exec, full, summary, outline"), *DetailLevel));
		}
	}

	int32 MaxNodes = 100;
	if (Arguments->HasField(TEXT("max_nodes")))
	{
		MaxNodes = static_cast<int32>(Arguments->GetNumberField(TEXT("max_nodes")));
	}

	FString AnchorGuid;
	if (Arguments->HasField(TEXT("anchor_node_guid")))
	{
		AnchorGuid = Arguments->GetStringField(TEXT("anchor_node_guid"));
	}

	int32 TraversalDepth = -1;
	if (Arguments->HasField(TEXT("traversal_depth")))
	{
		TraversalDepth = static_cast<int32>(Arguments->GetNumberField(TEXT("traversal_depth")));
	}

	// Load Blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprintFromPath(AssetPath, LoadError);
	if (!Blueprint)
	{
		return MakeErrorResult(LoadError);
	}

	// Collect graphs to process
	TArray<UEdGraph*> GraphsToProcess;
	if (!GraphName.IsEmpty())
	{
		FString FindError;
		UEdGraph* FoundGraph = FindGraphByName(Blueprint, GraphName, FindError);
		if (!FoundGraph)
		{
			return MakeErrorResult(FindError);
		}
		GraphsToProcess.Add(FoundGraph);
	}
	else
	{
		// All graphs: event graphs + function graphs
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (Graph)
			{
				GraphsToProcess.Add(Graph);
			}
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (Graph)
			{
				GraphsToProcess.Add(Graph);
			}
		}
	}

	if (GraphsToProcess.Num() == 0)
	{
		return MakeErrorResult(TEXT("No graphs found in Blueprint"));
	}

	// Build graphs array
	TArray<TSharedPtr<FJsonValue>> GraphsArray;
	int32 TotalNodeCount = 0;
	int32 TotalConnectionCount = 0;

	for (UEdGraph* Graph : GraphsToProcess)
	{
		if (!Graph)
		{
			continue;
		}

		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("graph_name"), Graph->GetName());

		// Build nodes array
		TArray<TSharedPtr<FJsonValue>> NodesArray;
		const int32 MaxNodesToShow = (MaxNodes > 0) ? MaxNodes : Graph->Nodes.Num();

		// Determine which nodes to include (BFS anchor or linear scan)
		TArray<UEdGraphNode*> NodesToProcess;

		if (!AnchorGuid.IsEmpty())
		{
			// BFS from anchor — parse GUID to handle both hyphenated and non-hyphenated formats
			FGuid ParsedAnchorGuid;
			FGuid::Parse(AnchorGuid, ParsedAnchorGuid);
			UEdGraphNode* AnchorNode = ParsedAnchorGuid.IsValid()
				? ClaireonBlueprintHelpers::FindNodeByGuid(Graph, ParsedAnchorGuid)
				: nullptr;

			if (AnchorNode)
			{
				const int32 BFSCap = FMath::Min((MaxNodes > 0) ? MaxNodes : 50, 50);
				TSet<FGuid> Visited;
				TArray<TPair<UEdGraphNode*, int32>> Frontier;
				Visited.Add(AnchorNode->NodeGuid);
				Frontier.Add(TPair<UEdGraphNode*, int32>(AnchorNode, 0));
				NodesToProcess.Add(AnchorNode);

				int32 FrontierIdx = 0;
				while (FrontierIdx < Frontier.Num() && NodesToProcess.Num() < BFSCap)
				{
					TPair<UEdGraphNode*, int32> Current = Frontier[FrontierIdx++];
					UEdGraphNode* CurrentNode = Current.Key;
					int32 CurrentDepth = Current.Value;

					if (TraversalDepth >= 0 && CurrentDepth >= TraversalDepth)
					{
						continue;
					}

					for (UEdGraphPin* Pin : CurrentNode->Pins)
					{
						if (!Pin || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
						{
							continue;
						}
						for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
						{
							if (!LinkedPin)
							{
								continue;
							}
							UEdGraphNode* Neighbor = LinkedPin->GetOwningNode();
							if (!Neighbor || Visited.Contains(Neighbor->NodeGuid))
							{
								continue;
							}
							Visited.Add(Neighbor->NodeGuid);
							NodesToProcess.Add(Neighbor);
							Frontier.Add(TPair<UEdGraphNode*, int32>(Neighbor, CurrentDepth + 1));
							if (NodesToProcess.Num() >= BFSCap)
							{
								break;
							}
						}
						if (NodesToProcess.Num() >= BFSCap)
						{
							break;
						}
					}
				}
			}
		}
		else
		{
			// Linear scan up to MaxNodesToShow
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node)
				{
					NodesToProcess.Add(Node);
					if (NodesToProcess.Num() >= MaxNodesToShow)
					{
						break;
					}
				}
			}
		}

		for (UEdGraphNode* Node : NodesToProcess)
		{
			if (!Node)
			{
				continue;
			}

			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			NodeObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString());
			NodeObj->SetStringField(TEXT("node_class"), Node->GetClass()->GetName());
			NodeObj->SetStringField(TEXT("node_title"), GetNodeTitle(Node));

			TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
			PosObj->SetNumberField(TEXT("x"), Node->NodePosX);
			PosObj->SetNumberField(TEXT("y"), Node->NodePosY);
			NodeObj->SetObjectField(TEXT("position"), PosObj);

			// Build pins array
			TArray<TSharedPtr<FJsonValue>> PinsArray;
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}

				TSharedPtr<FJsonObject> PinObj = MakeShared<FJsonObject>();
				PinObj->SetStringField(TEXT("pin_name"), Pin->PinName.ToString());
				PinObj->SetStringField(TEXT("pin_type"), GetPinTypeString(Pin));
				PinObj->SetStringField(TEXT("direction"), Pin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
				PinObj->SetNumberField(TEXT("connection_count"), Pin->LinkedTo.Num());

				if (DetailLevel == TEXT("full") && !Pin->DefaultValue.IsEmpty() && Pin->LinkedTo.Num() == 0)
				{
					PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
				}

				PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			NodeObj->SetArrayField(TEXT("pins"), PinsArray);

			NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
		}

		// Build connections array
		TArray<TSharedPtr<FJsonValue>> ConnectionsArray;
		TSet<UEdGraphNode*> ProcessedSet(NodesToProcess);
		for (UEdGraphNode* Node : NodesToProcess)
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
					// Only emit connections where both nodes are in our set
					if (!ProcessedSet.Contains(LinkedPin->GetOwningNode()))
					{
						continue;
					}

					TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
					ConnObj->SetStringField(TEXT("from_node"), Node->NodeGuid.ToString());
					ConnObj->SetStringField(TEXT("from_pin"), Pin->PinName.ToString());
					ConnObj->SetStringField(TEXT("to_node"), LinkedPin->GetOwningNode()->NodeGuid.ToString());
					ConnObj->SetStringField(TEXT("to_pin"), LinkedPin->PinName.ToString());
					ConnectionsArray.Add(MakeShared<FJsonValueObject>(ConnObj));
				}
			}
		}

		GraphObj->SetArrayField(TEXT("nodes"), NodesArray);
		GraphObj->SetNumberField(TEXT("node_count"), NodesArray.Num());
		GraphObj->SetArrayField(TEXT("connections"), ConnectionsArray);
		GraphObj->SetNumberField(TEXT("connection_count"), ConnectionsArray.Num());
		GraphObj->SetNumberField(TEXT("total_nodes_in_graph"), Graph->Nodes.Num());

		TotalNodeCount += NodesArray.Num();
		TotalConnectionCount += ConnectionsArray.Num();

		GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
	}

	// Build result data
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("blueprint_path"), AssetPath);
	Data->SetArrayField(TEXT("graphs"), GraphsArray);
	Data->SetNumberField(TEXT("graph_count"), GraphsArray.Num());

	// Summary
	FString Summary;
	if (GraphsArray.Num() == 1)
	{
		// Single graph: include graph name in summary
		FString SingleGraphName;
		if (!GraphName.IsEmpty())
		{
			SingleGraphName = GraphName;
		}
		else if (GraphsToProcess.Num() > 0 && GraphsToProcess[0])
		{
			SingleGraphName = GraphsToProcess[0]->GetName();
		}
		Summary = FString::Printf(TEXT("%s: %d nodes, %d connections"), *SingleGraphName, TotalNodeCount, TotalConnectionCount);
	}
	else
	{
		Summary = FString::Printf(TEXT("%d graphs: %d nodes, %d connections"), GraphsArray.Num(), TotalNodeCount, TotalConnectionCount);
	}

	return MakeSuccessResult(Data, Summary);
}

UBlueprint* ClaireonTool_GetBlueprintGraph::LoadBlueprintFromPath(const FString& AssetPath, FString& OutError)
{
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		OutError = FString::Printf(TEXT("Failed to load Blueprint at path: %s. Ensure the path is correct and the asset is a Blueprint."), *AssetPath);
		return nullptr;
	}

	return Blueprint;
}

bool ClaireonTool_GetBlueprintGraph::ValidateAssetPath(const FString& AssetPath, FString& OutError)
{
	if (!AssetPath.StartsWith(TEXT("/Game/")))
	{
		OutError = FString::Printf(TEXT("Asset path must start with /Game/. Got: %s"), *AssetPath);
		return false;
	}
	return true;
}

UEdGraph* ClaireonTool_GetBlueprintGraph::FindGraphByName(const UBlueprint* Blueprint, const FString& GraphName, FString& OutError)
{
	// Search in UbergraphPages (event graphs)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Search in FunctionGraphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	OutError = FString::Printf(TEXT("Graph '%s' not found in Blueprint. Use editor.blueprint.getProperties to see available graphs."), *GraphName);
	return nullptr;
}

FString ClaireonTool_GetBlueprintGraph::BuildGraphJsonSummary(const UEdGraph* Graph, const FString& DetailLevel, int32 MaxNodes, const FString& AnchorGuid, int32 TraversalDepth)
{
	if (!Graph)
	{
		return TEXT("Error: Invalid graph");
	}

	// AnimBP graphs don't use PC_Exec pins â fall back to "summary" detail
	FString EffectiveDetailLevel = DetailLevel;
	FString AnimBPFallbackNote;
	if (DetailLevel == TEXT("exec") && Graph->Schema && Graph->Schema->IsChildOf(UAnimationGraphSchema::StaticClass()))
	{
		EffectiveDetailLevel = TEXT("summary");
		AnimBPFallbackNote = TEXT("\n(exec detail not available for AnimBP graphs â showing summary)");
	}

	FString Output;
	Output += FString::Printf(TEXT("## Graph: %s (%d nodes)\n\n"), *Graph->GetName(), Graph->Nodes.Num());
	Output += TEXT("### Nodes\n\n");

	// --- Anchor BFS path ---
	if (!AnchorGuid.IsEmpty())
	{
		// Find the anchor node by GUID — parse to handle both hyphenated and non-hyphenated formats
		FGuid ParsedAnchorGuid;
		FGuid::Parse(AnchorGuid, ParsedAnchorGuid);
		UEdGraphNode* AnchorNode = ParsedAnchorGuid.IsValid()
			? ClaireonBlueprintHelpers::FindNodeByGuid(Graph, ParsedAnchorGuid)
			: nullptr;

		if (!AnchorNode)
		{
			return FString::Printf(TEXT("Anchor node not found: %s"), *AnchorGuid);
		}

		// BFS along exec pins from anchor
		// Cap at min(MaxNodes, 50) when anchor is used
		const int32 BFSCap = FMath::Min((MaxNodes > 0) ? MaxNodes : 50, 50);

		TArray<UEdGraphNode*> CollectedNodes;
		TSet<FGuid> Visited;
		// Queue entries: (node, depth)
		TArray<TPair<UEdGraphNode*, int32>> Frontier;

		Visited.Add(AnchorNode->NodeGuid);
		Frontier.Add(TPair<UEdGraphNode*, int32>(AnchorNode, 0));
		CollectedNodes.Add(AnchorNode);

		int32 FrontierIdx = 0;
		while (FrontierIdx < Frontier.Num() && CollectedNodes.Num() < BFSCap)
		{
			TPair<UEdGraphNode*, int32> Current = Frontier[FrontierIdx++];
			UEdGraphNode* CurrentNode = Current.Key;
			int32 CurrentDepth = Current.Value;

			// Check depth limit
			if (TraversalDepth >= 0 && CurrentDepth >= TraversalDepth)
			{
				continue;
			}

			// Follow exec pins in both directions
			for (UEdGraphPin* Pin : CurrentNode->Pins)
			{
				if (!Pin || Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
				{
					continue;
				}

				for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
				{
					if (!LinkedPin)
					{
						continue;
					}
					UEdGraphNode* Neighbor = LinkedPin->GetOwningNode();
					if (!Neighbor || Visited.Contains(Neighbor->NodeGuid))
					{
						continue;
					}

					Visited.Add(Neighbor->NodeGuid);
					CollectedNodes.Add(Neighbor);
					Frontier.Add(TPair<UEdGraphNode*, int32>(Neighbor, CurrentDepth + 1));

					if (CollectedNodes.Num() >= BFSCap)
					{
						break;
					}
				}

				if (CollectedNodes.Num() >= BFSCap)
				{
					break;
				}
			}
		}

		// Fallback: if anchor has no exec connections, show data-pin neighbors at "outline" detail
		if (CollectedNodes.Num() == 1)
		{
			// Check if anchor truly has no exec pins
			bool bHasExecPins = false;
			for (UEdGraphPin* Pin : AnchorNode->Pins)
			{
				if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
				{
					bHasExecPins = true;
					break;
				}
			}

			if (!bHasExecPins)
			{
				// Show anchor at requested detail, data-pin neighbors at "outline"
				Output += FString::Printf(TEXT("1. %s\n"), *FormatNodeSummary(AnchorNode, EffectiveDetailLevel));

				int32 NeighborIdx = 2;
				TSet<FGuid> DataNeighborVisited;
				DataNeighborVisited.Add(AnchorNode->NodeGuid);

				for (UEdGraphPin* Pin : AnchorNode->Pins)
				{
					if (!Pin)
					{
						continue;
					}
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (!LinkedPin)
						{
							continue;
						}
						UEdGraphNode* Neighbor = LinkedPin->GetOwningNode();
						if (!Neighbor || DataNeighborVisited.Contains(Neighbor->NodeGuid))
						{
							continue;
						}
						DataNeighborVisited.Add(Neighbor->NodeGuid);
						Output += FString::Printf(TEXT("%d. %s\n"), NeighborIdx++, *FormatNodeSummary(Neighbor, TEXT("outline")));
					}
				}

				FString AnchorTitle = GetNodeTitle(AnchorNode);
				Output += FString::Printf(TEXT("\n(Anchor node [%s] has no exec connections â showing data-pin neighbors instead.)"), *AnchorTitle);
				Output += AnimBPFallbackNote;
				return Output;
			}
		}

		// Render collected BFS nodes
		int32 NodeIndex = 1;
		for (UEdGraphNode* Node : CollectedNodes)
		{
			Output += FString::Printf(TEXT("%d. %s\n"), NodeIndex++, *FormatNodeSummary(Node, EffectiveDetailLevel));
		}

		// Navigation footer
		int32 TotalNodes = Graph->Nodes.Num();
		int32 ShownNodes = CollectedNodes.Num();
		int32 NotShown = TotalNodes - ShownNodes;
		FString AnchorTitle = GetNodeTitle(AnchorNode);
		FString DepthStr = (TraversalDepth >= 0) ? FString::FromInt(TraversalDepth) : TEXT("unlimited");
		Output += FString::Printf(
			TEXT("\n(Showing %d of %d nodes â anchored at [%s], depth %s.\n %d nodes not shown. Use anchor_node_guid=<guid> to navigate to a different section.)"),
			ShownNodes, TotalNodes, *AnchorTitle, *DepthStr, NotShown);
		Output += AnimBPFallbackNote;
		return Output;
	}

	// --- Normal (non-anchor) path ---
	int32 NodeIndex = 1;
	int32 NodesShown = 0;
	const int32 MaxNodesToShow = (MaxNodes > 0) ? MaxNodes : Graph->Nodes.Num();

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		if (NodesShown >= MaxNodesToShow)
		{
			break;
		}

		FString NodeSummary = FormatNodeSummary(Node, EffectiveDetailLevel);
		Output += FString::Printf(TEXT("%d. %s\n"), NodeIndex, *NodeSummary);

		NodeIndex++;
		NodesShown++;
	}

	// Add overflow summary if needed
	if (Graph->Nodes.Num() > MaxNodesToShow)
	{
		Output += TEXT("\n");
		Output += BuildOverflowSummary(Graph, NodesShown, Graph->Nodes.Num());
	}
	else
	{
		Output += FString::Printf(TEXT("\n(Showing %d of %d nodes at '%s' detail)"), NodesShown, Graph->Nodes.Num(), *EffectiveDetailLevel);
	}

	Output += AnimBPFallbackNote;
	return Output;
}

FString ClaireonTool_GetBlueprintGraph::BuildGraphT3DExport(const UEdGraph* Graph)
{
	if (!Graph)
	{
		return TEXT("Error: Invalid graph");
	}

	// Collect all nodes to export
	TSet<UObject*> NodesToExport;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (Node)
		{
			NodesToExport.Add(Node);
		}
	}

	if (NodesToExport.Num() == 0)
	{
		return TEXT("Error: No nodes to export");
	}

	// Export using Unreal's T3D serialization
	FString ExportedText;
	FEdGraphUtilities::ExportNodesToText(NodesToExport, ExportedText);

	return ExportedText;
}

FString ClaireonTool_GetBlueprintGraph::FormatNodeSummary(const UEdGraphNode* Node, const FString& DetailLevel)
{
	if (!Node)
	{
		return TEXT("[Invalid Node]");
	}

	FString NodeTitle = GetNodeTitle(Node);
	FString NodeClass = Node->GetClass()->GetName();
	FVector2D NodePos(Node->NodePosX, Node->NodePosY);

	FString Summary = FString::Printf(TEXT("[%s] (%s) @ (%.0f, %.0f)"),
		*NodeTitle, *NodeClass, NodePos.X, NodePos.Y);

	// Add GUID for reference
	if (DetailLevel == TEXT("full") || DetailLevel == TEXT("summary") || DetailLevel == TEXT("exec"))
	{
		Summary += FString::Printf(TEXT(" [GUID: %s]"), *Node->NodeGuid.ToString());
	}

	if (DetailLevel == TEXT("outline"))
	{
		// Outline mode: just the title and class
		return Summary;
	}

	if (DetailLevel == TEXT("exec"))
	{
		// Exec mode: show only exec-pin connections and a compact data-pin count
		int32 DataPinCount = 0;

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}

			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				// Only show exec pins that have connections
				if (Pin->LinkedTo.Num() > 0)
				{
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode())
						{
							FString ConnectedTitle = GetNodeTitle(LinkedPin->GetOwningNode());
							if (Pin->Direction == EGPD_Input)
							{
								Summary += FString::Printf(TEXT("\n   <- %s <- [%s]"), *Pin->PinName.ToString(), *ConnectedTitle);
							}
							else
							{
								Summary += FString::Printf(TEXT("\n   -> %s -> [%s]"), *Pin->PinName.ToString(), *ConnectedTitle);
							}
						}
					}
				}
			}
			else
			{
				DataPinCount++;
			}
		}

		// Compact data pin count (omit if zero)
		if (DataPinCount > 0)
		{
			Summary += FString::Printf(TEXT("\n   (%d data pins)"), DataPinCount);
		}

		return Summary;
	}

	// Add pin information for summary and full modes
	TArray<FString> InputPins;
	TArray<FString> OutputPins;

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin)
		{
			continue;
		}

		FString PinInfo = FormatPinInfo(Pin, DetailLevel);

		if (Pin->Direction == EGPD_Input)
		{
			InputPins.Add(PinInfo);
		}
		else if (Pin->Direction == EGPD_Output)
		{
			OutputPins.Add(PinInfo);
		}
	}

	// Format inputs
	if (InputPins.Num() > 0)
	{
		Summary += TEXT("\n   Inputs: ");
		Summary += FString::Join(InputPins, TEXT(", "));
	}

	// Format outputs
	if (OutputPins.Num() > 0)
	{
		Summary += TEXT("\n   Outputs: ");
		Summary += FString::Join(OutputPins, TEXT(", "));
	}

	return Summary;
}

FString ClaireonTool_GetBlueprintGraph::FormatPinInfo(const UEdGraphPin* Pin, const FString& DetailLevel)
{
	if (!Pin)
	{
		return TEXT("[Invalid Pin]");
	}

	FString PinInfo = Pin->PinName.ToString();

	// Add type info
	FString TypeString = GetPinTypeString(Pin);
	if (!TypeString.IsEmpty())
	{
		PinInfo += FString::Printf(TEXT("(%s)"), *TypeString);
	}

	// Add connection info
	if (Pin->LinkedTo.Num() > 0)
	{
		if (Pin->Direction == EGPD_Input)
		{
			PinInfo += TEXT(" <- ");
		}
		else
		{
			PinInfo += TEXT(" -> ");
		}

		TArray<FString> LinkedNodes;
		for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
		{
			if (LinkedPin && LinkedPin->GetOwningNode())
			{
				LinkedNodes.Add(FString::Printf(TEXT("[%s]"), *GetNodeTitle(LinkedPin->GetOwningNode())));
			}
		}
		PinInfo += FString::Join(LinkedNodes, TEXT(", "));
	}
	else if (DetailLevel == TEXT("full") && !Pin->DefaultValue.IsEmpty())
	{
		// Show default value in full mode
		PinInfo += FString::Printf(TEXT(" = %s"), *Pin->DefaultValue);
	}
	else if (Pin->LinkedTo.Num() == 0)
	{
		PinInfo += TEXT(" -> (none)");
	}

	return PinInfo;
}

FString ClaireonTool_GetBlueprintGraph::BuildOverflowSummary(const UEdGraph* Graph, int32 NumShown, int32 TotalNodes)
{
	int32 Remaining = TotalNodes - NumShown;

	// Count remaining nodes by type
	TMap<FString, int32> NodeTypeCounts;
	int32 NodesProcessed = 0;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		NodesProcessed++;
		if (NodesProcessed <= NumShown)
		{
			continue;
		}

		FString NodeClass = Node->GetClass()->GetName();
		int32* Count = NodeTypeCounts.Find(NodeClass);
		if (Count)
		{
			(*Count)++;
		}
		else
		{
			NodeTypeCounts.Add(NodeClass, 1);
		}
	}

	// Format the summary
	FString Summary = FString::Printf(TEXT("(Showing first %d of %d nodes. Remaining %d nodes: "),
		NumShown, TotalNodes, Remaining);

	TArray<FString> TypeSummaries;
	for (const auto& Pair : NodeTypeCounts)
	{
		TypeSummaries.Add(FString::Printf(TEXT("%d %s"), Pair.Value, *Pair.Key));
	}

	Summary += FString::Join(TypeSummaries, TEXT(", "));
	Summary += TEXT(")");

	return Summary;
}

FString ClaireonTool_GetBlueprintGraph::GetPinTypeString(const UEdGraphPin* Pin)
{
	if (!Pin)
	{
		return TEXT("");
	}

	const FEdGraphPinType& PinType = Pin->PinType;
	FString TypeString;

	// Get the base type category
	if (PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
	{
		TypeString = TEXT("Exec");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Boolean)
	{
		TypeString = TEXT("Bool");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Byte)
	{
		TypeString = TEXT("Byte");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int)
	{
		TypeString = TEXT("Int");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Int64)
	{
		TypeString = TEXT("Int64");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Real)
	{
		if (PinType.PinSubCategory == UEdGraphSchema_K2::PC_Float)
		{
			TypeString = TEXT("Float");
		}
		else
		{
			TypeString = TEXT("Double");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Name)
	{
		TypeString = TEXT("Name");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_String)
	{
		TypeString = TEXT("String");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Text)
	{
		TypeString = TEXT("Text");
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Object || PinType.PinCategory == UEdGraphSchema_K2::PC_Class)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			TypeString = PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			TypeString = TEXT("Object");
		}
	}
	else if (PinType.PinCategory == UEdGraphSchema_K2::PC_Struct)
	{
		if (PinType.PinSubCategoryObject.IsValid())
		{
			TypeString = PinType.PinSubCategoryObject->GetName();
		}
		else
		{
			TypeString = TEXT("Struct");
		}
	}
	else
	{
		TypeString = PinType.PinCategory.ToString();
	}

	// Add container type
	if (PinType.IsArray())
	{
		TypeString = FString::Printf(TEXT("Array<%s>"), *TypeString);
	}
	else if (PinType.IsSet())
	{
		TypeString = FString::Printf(TEXT("Set<%s>"), *TypeString);
	}
	else if (PinType.IsMap())
	{
		TypeString = FString::Printf(TEXT("Map<%s>"), *TypeString);
	}

	return TypeString;
}

FString ClaireonTool_GetBlueprintGraph::GetNodeTitle(const UEdGraphNode* Node)
{
	if (!Node)
	{
		return TEXT("Unknown");
	}

	FText NodeTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle);
	return NodeTitle.ToString();
}

bool ClaireonTool_GetBlueprintGraph::IsValidDetailLevel(const FString& DetailLevel)
{
	return DetailLevel == TEXT("full") || DetailLevel == TEXT("summary") || DetailLevel == TEXT("outline") || DetailLevel == TEXT("exec");
}
