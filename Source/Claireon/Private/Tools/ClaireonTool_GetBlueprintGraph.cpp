// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_GetBlueprintGraph.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h" // kBPCategory
#include "ClaireonPathResolver.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonNameResolver.h"
#include "ClaireonLog.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node.h" // UK2Node cast at line 484 (N7 pure-call subgraph traversal)
#include "EdGraphUtilities.h"
#include "ClaireonBlueprintNodeSerializer.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraphSchema.h"
#include "WidgetBlueprint.h"
#include "EdGraphNode_Comment.h"

FString ClaireonTool_GetBlueprintGraph::GetCategory() const { return kBPCategory; }
FString ClaireonTool_GetBlueprintGraph::GetOperation() const { return TEXT("get_graph"); }

TArray<FString> ClaireonTool_GetBlueprintGraph::GetSearchKeywords() const
{
	return {TEXT("bp"), TEXT("blueprint"), TEXT("graph"), TEXT("get"), TEXT("read"), TEXT("inspect"), TEXT("t3d"), TEXT("outline")};
}

FString ClaireonTool_GetBlueprintGraph::GetDescription() const
{
	return TEXT("Read Blueprint graph structure at configurable detail levels (exec/full/summary/outline). Supports BFS traversal from anchor nodes and JSON or T3D export formats. Immediate-mode tool: no session required.");
}

FString ClaireonTool_GetBlueprintGraph::GetFullDescription() const
{
	return TEXT("Read the internal graph structure of a Blueprint.\n"
				"Default output: JSON summary at 'exec' detail level - node titles, classes, GUIDs, positions, exec-pin connections, and compact data-pin counts. Suitable for surveying large graphs without token overflow.\n\n"
				"Parameters:\n"
				"  asset_path (required): Unreal content path of the Blueprint (e.g., /Game/Characters/BP_PlayerCharacter).\n"
				"  graph_name (optional): Name of a specific graph to export (e.g., 'EventGraph', 'TakeDamage'). Omit to export all graphs.\n"
				"  format (optional, default='json'): 'json' for structured summary, 't3d' for Unreal text export (clipboard copy/paste), 'both' for both. T3D is opt-in.\n"
				"  node_detail_level (optional, default='exec'):\n"
				"    'exec' - compact view: title, class, GUID, position, exec-pin connections, and compact data-pin count. Best for large graphs.\n"
				"    'full' - all pin details and defaults.\n"
				"    'summary' - node types and connections only (no pin defaults).\n"
				"    'outline' - one line per node, parseable by a single regex. No embedded newlines.\n"
				"      Grammar: '<index>. <node_class> <guid8>  <clean_title>  @ (x, y)'\n"
				"      Regex:   ^\\s*(\\d+)\\.\\s+(\\w+)\\s+([0-9a-fA-F]{8})\\s{2}(.+?)\\s{2}@\\s+\\((-?\\d+),\\s*(-?\\d+)\\)\\s*$\n"
				"      <guid8> is the first 8 hex chars of the node GUID; full GUID included in exec/summary/full.\n"
				"      <clean_title> uses ENodeTitleType::ListView; embedded newlines replaced with spaces.\n"
				"    At 'full' detail the JSON payload additionally includes an optional 'node_subtitle' field\n"
				"      (e.g. 'Target is Kismet System Library') when the node FullTitle has a second line.\n"
				"  include_pin_defaults (optional, default=true): Include default values for unconnected pins at 'full' and 'summary' detail. Pass false to suppress. 'exec'/'outline' never emit defaults.\n"
				"  max_nodes (optional, default=0 unlimited): Maximum nodes to include. 0 means all nodes. Capped at 50 when anchor_node_guid is used.\n"
				"  anchor_node_guid (optional): GUID of a node to anchor BFS traversal. When provided, returns only nodes reachable via exec connections from this node (up to 50). "
				"Use node_detail_level='exec' on the full graph first to get GUIDs, then anchor + node_detail_level='full' to drill into a specific section.\n"
				"  exec_only (optional, default=false): When true, suppress pure-subgraph expansion even when anchor_node_guid would otherwise default it on. The specialist walk (see include_pure_subgraph) never runs. Anchored calls include pure feeder nodes by default; pass exec_only=true for the exec chain alone.\n"
				"  traversal_depth (optional, default=-1): BFS hop limit when anchor_node_guid is set. 0=anchor only, 1=anchor+direct neighbors, 2=two hops, -1=unlimited.\n"
				"  filter_class (optional): Server-side class filter, e.g. 'K2Node_CallFunction'. Matches subclasses too.\n"
				"  filter_title_contains (optional): Case-insensitive substring match against node_title.\n"
				"  offset (optional, default=0): Skip this many nodes of the filtered set before applying max_nodes. Ignored when anchor_node_guid is set.\n\n"
				"Payload contracts:\n"
				"- All node GUIDs in the payload (nodes[].node_id, connections[].from_node/to_node, pins[].linked_to[].node_id) use ONE format: digits with hyphens (e.g. A1B2C3D4-...). They string-match each other directly.\n"
				"- At 'full' and 'exec' detail each connected pin carries linked_to: [{node_id, pin_name}] listing its actual endpoints.\n"
				"- connections[] never drops edges: when max_nodes/anchor truncation leaves an edge's other node outside the returned set, the edge is kept and flagged target_in_set:false (outgoing) or source_in_set:false (incoming). Absence of the flag means both endpoints are in nodes[].\n"
				"- Each graph object carries total_filtered: the number of nodes passing node_filter/filter_class/filter_title_contains BEFORE offset/max_nodes windowing. Page with offset until offset >= total_filtered.\n\n"
				"Navigation workflow: Call with defaults to get a compact exec-level overview and node GUIDs. Then use anchor_node_guid=<guid> with node_detail_level='full' to inspect a specific subgraph.\n"
				"Use bp_get_properties first to discover available graphs.\n\n"
				"node_type_alias and generic_class_name (roundtrip contract for add_node):\n"
				"- node_type_alias is the guaranteed-roundtrip value for add_node(node_type=...). Prefer it\n"
				"  over node_class when re-creating nodes.\n"
				"- When node_type_alias == 'Generic', the caller MUST also pass generic_class_name as\n"
				"  add_node(class_name=<that>). Passing only node_type='Generic' without class_name hits\n"
				"  the terminal Unsupported node type error.\n"
				"- node_class remains the raw UClass name (engine namespace) for callers that need it.\n"
				"  Newer callers should prefer node_type_alias for replay; node_class is retained for\n"
				"  back-compat.");
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
	DefaultsProp->SetStringField(TEXT("description"), TEXT("Include default values for unconnected pins in JSON output. Default: true. Applies at 'full' and 'summary' node_detail_level (both emit defaults unless this is explicitly false). At 'exec' and 'outline' detail no pin defaults are emitted regardless of this flag."));
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
														 "'exec' - compact view: node title, class, GUID, position, exec-pin connections only, and a compact data pin count (N data pins). Best for surveying large graphs.\n"
														 "'full' - all pin details and defaults.\n"
														 "'summary' - node types and connections only (no pin defaults).\n"
														 "'outline' - one line per node; see full description for grammar and regex."));
	Properties->SetObjectField(TEXT("node_detail_level"), DetailProp);

	// max_nodes - optional
	TSharedPtr<FJsonObject> MaxProp = MakeShared<FJsonObject>();
	MaxProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxProp->SetStringField(TEXT("description"), TEXT("Maximum number of nodes to return. 0 (default) = unlimited (all nodes). Pass a positive integer to cap. Capped at 50 when anchor_node_guid is used."));
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

	// node_filter - optional triage filter for very large graphs.
	TSharedPtr<FJsonObject> NodeFilterProp = MakeShared<FJsonObject>();
	NodeFilterProp->SetStringField(TEXT("type"), TEXT("string"));
	TArray<TSharedPtr<FJsonValue>> NodeFilterEnum;
	NodeFilterEnum.Add(MakeShared<FJsonValueString>(TEXT("all")));
	NodeFilterEnum.Add(MakeShared<FJsonValueString>(TEXT("entry_points")));
	NodeFilterEnum.Add(MakeShared<FJsonValueString>(TEXT("comments")));
	NodeFilterProp->SetArrayField(TEXT("enum"), NodeFilterEnum);
	NodeFilterProp->SetStringField(TEXT("description"), TEXT("Pre-filter nodes before applying node_detail_level. "
															 "'all' (default) emits every node. "
															 "'entry_points' emits only nodes that begin an exec flow -- events, custom events, function entries, and any other node with zero connected input exec pins. "
															 "'comments' emits only UEdGraphNode_Comment nodes (their bounding-box rectangles act as section labels in the editor). "
															 "Use 'entry_points' or 'comments' to triage very large graphs (e.g. BP_FSAIController EventGraph) before pulling full detail."));
	Properties->SetObjectField(TEXT("node_filter"), NodeFilterProp);

	// include_pure_subgraph
	TSharedPtr<FJsonObject> PureProp = MakeShared<FJsonObject>();
	PureProp->SetStringField(TEXT("type"), TEXT("boolean"));
	PureProp->SetStringField(TEXT("description"),
		TEXT("When true, expand the result to include pure-call source nodes "
			 "(K2Node_PromotableOperator / math libs / getters) that feed data-input "
			 "pins on selected nodes, and emit edges from each pure source into the "
			 "consuming node. Default false. Most useful with node_detail_level='full' "
			 "for translator workflows that need to see how each data pin is computed."
			 " DEPRECATED alias: exec_only=true is equivalent to include_pure_subgraph=false and is preferred for anchored calls; this flag is retained for back-compat and default-false unanchored calls."));
	Properties->SetObjectField(TEXT("include_pure_subgraph"), PureProp);

	// exec_only - force-suppress pure-subgraph expansion on anchored calls.
	TSharedPtr<FJsonObject> ExecOnlyProp = MakeShared<FJsonObject>();
	ExecOnlyProp->SetStringField(TEXT("type"), TEXT("boolean"));
	ExecOnlyProp->SetStringField(TEXT("description"),
		TEXT("When true, suppress pure-subgraph expansion even when anchor_node_guid would otherwise default it on. "
			 "The specialist walk (see include_pure_subgraph) never runs. Default false."));
	Properties->SetObjectField(TEXT("exec_only"), ExecOnlyProp);

	// filter_class - server-side class filter.
	TSharedPtr<FJsonObject> FilterClassProp = MakeShared<FJsonObject>();
	FilterClassProp->SetStringField(TEXT("type"), TEXT("string"));
	FilterClassProp->SetStringField(TEXT("description"),
		TEXT("Filter nodes by class name (e.g. 'K2Node_CallFunction'). Resolved via ClaireonNameResolver::ResolveClassName "
			 "against UEdGraphNode as the required base class (same fuzzy resolution used elsewhere in Claireon for bare class "
			 "names -- accepts short names, case-insensitive, U-prefixed or not). A node matches if its actual class IsChildOf "
			 "the resolved class, so filtering by a base class also returns its subclasses. If the name does not resolve to "
			 "exactly one class, returns an error naming the resolver's own candidates/error text verbatim."));
	Properties->SetObjectField(TEXT("filter_class"), FilterClassProp);

	// filter_title_contains - server-side title substring filter.
	TSharedPtr<FJsonObject> FilterTitleProp = MakeShared<FJsonObject>();
	FilterTitleProp->SetStringField(TEXT("type"), TEXT("string"));
	FilterTitleProp->SetStringField(TEXT("description"),
		TEXT("Case-insensitive substring match against the node's ListView title (the same title text emitted as node_title "
			 "in the payload). A node matches if its node_title contains this substring, ignoring case."));
	Properties->SetObjectField(TEXT("filter_title_contains"), FilterTitleProp);

	// offset - pagination over the filtered set.
	TSharedPtr<FJsonObject> OffsetProp = MakeShared<FJsonObject>();
	OffsetProp->SetStringField(TEXT("type"), TEXT("integer"));
	OffsetProp->SetStringField(TEXT("description"),
		TEXT("Skip this many nodes from the front of the filtered set before applying max_nodes. Composes with "
			 "node_filter/filter_class/filter_title_contains: the offset is applied to the set AFTER all other filters, so two "
			 "calls with the same filters and offset=0/offset=max_nodes partition the same filtered result. "
			 "Ignored (treated as 0) when anchor_node_guid is set -- offset does not apply to BFS traversal order."));
	Properties->SetObjectField(TEXT("offset"), OffsetProp);

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
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

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

	// format: 'json' (default) | 't3d' | 'both'. T3D output attaches a per-graph
	// 't3d' field alongside (or instead of caring about) the JSON node dump.
	FString Format = TEXT("json");
	if (Arguments->HasField(TEXT("format")))
	{
		Format = Arguments->GetStringField(TEXT("format")).ToLower();
		if (Format != TEXT("json") && Format != TEXT("t3d") && Format != TEXT("both"))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid format: %s. Must be one of: json, t3d, both"), *Format));
		}
	}
	const bool bIncludeT3D = (Format == TEXT("t3d") || Format == TEXT("both"));

	// include_pin_defaults: when explicitly false, suppress default_value emission on
	// unconnected pins (and sub-pins). Default true, matching the long-standing output
	// shape at 'full' and 'summary' detail. 'exec'/'outline' never emit defaults.
	bool bIncludePinDefaults = true;
	if (Arguments->HasField(TEXT("include_pin_defaults")))
	{
		bIncludePinDefaults = Arguments->GetBoolField(TEXT("include_pin_defaults"));
	}

	int32 MaxNodes = 0;  // 0 = unlimited (all nodes); callers may pass a positive cap
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

	FString NodeFilter = TEXT("all");
	if (Arguments->HasField(TEXT("node_filter")))
	{
		NodeFilter = Arguments->GetStringField(TEXT("node_filter"));
		if (!(NodeFilter.Equals(TEXT("all"), ESearchCase::IgnoreCase)
			|| NodeFilter.Equals(TEXT("entry_points"), ESearchCase::IgnoreCase)
			|| NodeFilter.Equals(TEXT("comments"), ESearchCase::IgnoreCase)))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid node_filter: %s. Must be one of: all, entry_points, comments"), *NodeFilter));
		}
		NodeFilter = NodeFilter.ToLower();
	}

	// Pure-subgraph expansion pulls in the pure-call source nodes (K2Node_PromotableOperator,
	// math lib calls, getters) feeding data-input pins on already-selected nodes. Precedence
	// for the effective value, evaluated after AnchorGuid is read so anchor presence is known:
	//   1. exec_only=true            -> false (wins outright; the only force-suppress)
	//   2. explicit include_pure_subgraph -> that value (back-compat, either direction)
	//   3. anchored call             -> true (flipped default: anchored walks include feeders)
	//   4. otherwise                 -> false (unanchored dumps already list pure nodes via
	//                                   the linear scan; only the anchored BFS excluded them)
	bool bIncludePureSubgraph = false;
	if (Arguments->HasField(TEXT("exec_only")) && Arguments->GetBoolField(TEXT("exec_only")))
	{
		bIncludePureSubgraph = false;
	}
	else if (Arguments->HasField(TEXT("include_pure_subgraph")))
	{
		bIncludePureSubgraph = Arguments->GetBoolField(TEXT("include_pure_subgraph"));
	}
	else if (!AnchorGuid.IsEmpty())
	{
		bIncludePureSubgraph = true;
	}

	// filter_class -- resolved ONCE per Execute, not per node. Resolution failure is fatal:
	// returning an unfiltered result would silently ignore the caller's filter.
	UClass* FilterClass = nullptr;
	if (Arguments->HasField(TEXT("filter_class")))
	{
		const FString FilterClassStr = Arguments->GetStringField(TEXT("filter_class"));
		if (!FilterClassStr.IsEmpty())
		{
			ClaireonNameResolver::FNameResolveResult ClassResult;
			FilterClass = ClaireonNameResolver::ResolveClassName(FilterClassStr, UEdGraphNode::StaticClass(), ClassResult);
			if (!IsValid(FilterClass))
			{
				return MakeErrorResult(ClassResult.Error);
			}
		}
	}

	FString FilterTitleContains;
	if (Arguments->HasField(TEXT("filter_title_contains")))
	{
		FilterTitleContains = Arguments->GetStringField(TEXT("filter_title_contains"));
	}

	int32 Offset = 0;
	if (Arguments->HasField(TEXT("offset")))
	{
		Offset = FMath::Max(0, static_cast<int32>(Arguments->GetNumberField(TEXT("offset"))));
	}

	// node_filter predicate. Applied after the BFS / linear scan, before node serialization.
	// 'entry_points' = no connected input exec pin (events, custom events, function entries, etc.)
	// 'comments' = UEdGraphNode_Comment instances.
	// 'all' = pass-through.
	auto PassesNodeFilter = [&NodeFilter](UEdGraphNode* Node) -> bool
	{
		if (!IsValid(Node)) { return false; }
		if (NodeFilter == TEXT("all")) { return true; }
		if (NodeFilter == TEXT("comments"))
		{
			return Cast<UEdGraphNode_Comment>(Node) != nullptr;
		}
		// entry_points: zero connected input exec pins.
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin) { continue; }
			if (Pin->Direction != EGPD_Input) { continue; }
			if (Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec) { continue; }
			if (Pin->LinkedTo.Num() > 0)
			{
				return false;
			}
		}
		// Also require AT LEAST one exec pin (input or output) to count as a flow node;
		// otherwise pure data nodes (literals, getters) match, which is not useful.
		bool bHasAnyExec = false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				bHasAnyExec = true;
				break;
			}
		}
		// Special case: UEdGraphNode_Comment has no exec pins but IS a useful entry-point marker.
		if (Cast<UEdGraphNode_Comment>(Node))
		{
			return false; // comments are not entry points, even though they have no inputs
		}
		return bHasAnyExec;
	};

	// filter_class / filter_title_contains predicate, composed with PassesNodeFilter via &&.
	// Kept separate so the BFS path can tell "no extra filters requested" from "everything
	// failed them" when deciding whether the post-BFS filter pass is needed at all.
	const bool bHasExtraFilters = (FilterClass != nullptr) || !FilterTitleContains.IsEmpty();
	auto PassesExtraFilters = [FilterClass, &FilterTitleContains](UEdGraphNode* Node) -> bool
	{
		if (!IsValid(Node)) { return false; }
		if (IsValid(FilterClass) && !Node->GetClass()->IsChildOf(FilterClass)) { return false; }
		if (!FilterTitleContains.IsEmpty()
			&& !GetNodeTitle(Node).Contains(FilterTitleContains, ESearchCase::IgnoreCase))
		{
			return false;
		}
		return true;
	};

	// Load Blueprint
	FString LoadError;
	UBlueprint* Blueprint = LoadBlueprintFromPath(AssetPath, LoadError);
	if (!IsValid(Blueprint))
	{
		return MakeErrorResult(LoadError);
	}

	// Collect graphs to process
	TArray<UEdGraph*> GraphsToProcess;
	if (!GraphName.IsEmpty())
	{
		FString FindError;
		UEdGraph* FoundGraph = FindGraphByName(Blueprint, GraphName, FindError);
		if (!IsValid(FoundGraph))
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
			if (IsValid(Graph))
			{
				GraphsToProcess.Add(Graph);
			}
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (IsValid(Graph))
			{
				GraphsToProcess.Add(Graph);
			}
		}
	}

	if (GraphsToProcess.Num() == 0)
	{
		return MakeErrorResult(TEXT("No graphs found in Blueprint"));
	}

	// Resolve the anchor node up front via the shared >=8-hex prefix resolver.
	// An unresolvable, ambiguous, or malformed anchor is now a structured error
	// instead of a silent node_count=0 (WS-B B-4 / WS-C C-2). The anchor lives in
	// exactly one of the graphs being processed; find it there.
	UEdGraphNode* ResolvedAnchorNode = nullptr;
	if (!AnchorGuid.IsEmpty())
	{
		FString AnchorError = FString::Printf(
			TEXT("anchor_node_guid '%s' did not resolve to any node in the requested graph(s)."), *AnchorGuid);
		for (UEdGraph* Graph : GraphsToProcess)
		{
			if (!IsValid(Graph))
			{
				continue;
			}
			UEdGraphNode* Candidate = nullptr;
			FString Err;
			if (ClaireonBlueprintHelpers::ResolveNodeGuidString(Graph, AnchorGuid, Candidate, Err, TEXT("anchor_node_guid")))
			{
				ResolvedAnchorNode = Candidate;
				break;
			}
			AnchorError = Err;
		}
		if (!IsValid(ResolvedAnchorNode))
		{
			return MakeErrorResult(AnchorError);
		}
	}

	// Build graphs array
	TArray<TSharedPtr<FJsonValue>> GraphsArray;
	int32 TotalNodeCount = 0;
	int32 TotalConnectionCount = 0;

	for (UEdGraph* Graph : GraphsToProcess)
	{
		if (!IsValid(Graph))
		{
			continue;
		}

		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		// Emit the prefixed graph_name form only (no short-form `name` alias),
		// to align with the rest of the BP output surface (node_title,
		// node_class, node_id, etc.).
		GraphObj->SetStringField(TEXT("graph_name"), Graph->GetName());

		// Build nodes array
		TArray<TSharedPtr<FJsonValue>> NodesArray;

		// Determine which nodes to include (BFS anchor or linear scan)
		TArray<UEdGraphNode*> NodesToProcess;

		// Size of the filtered set BEFORE offset/max_nodes windowing -- what a caller has to
		// page through. Set by whichever path below runs.
		int32 TotalFiltered = 0;

		if (!AnchorGuid.IsEmpty())
		{
			// BFS from the anchor node (resolved up front via the shared prefix
			// resolver). Only the graph that actually owns the anchor runs the BFS;
			// other graphs in a multi-graph dump emit an empty node set as before.
			UEdGraphNode* AnchorNode = (IsValid(ResolvedAnchorNode) && Graph->Nodes.Contains(ResolvedAnchorNode))
				? ResolvedAnchorNode
				: nullptr;

			if (IsValid(AnchorNode))
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
							if (!IsValid(Neighbor) || Visited.Contains(Neighbor->NodeGuid))
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
			// Linear scan. All filters are applied first so the window (offset + max_nodes)
			// bounds the FILTERED set -- asking for 50 entry-points returns the first 50
			// entry-points, not the first 50 nodes narrowed to whatever happened to match.
			// The full filtered list is materialized because total_filtered and offset both
			// need its true size, which an early break would hide.
			TArray<UEdGraphNode*> FilteredNodes;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (IsValid(Node) && PassesNodeFilter(Node) && PassesExtraFilters(Node))
				{
					FilteredNodes.Add(Node);
				}
			}
			TotalFiltered = FilteredNodes.Num();

			const int32 WindowStart = FMath::Min(Offset, FilteredNodes.Num());
			const int32 Available = FilteredNodes.Num() - WindowStart;
			const int32 WindowCount = (MaxNodes > 0) ? FMath::Min(MaxNodes, Available) : Available;
			NodesToProcess.Reserve(WindowCount);
			for (int32 I = WindowStart; I < WindowStart + WindowCount; ++I)
			{
				NodesToProcess.Add(FilteredNodes[I]);
			}
		}

		// BFS path: filters are applied post-BFS so the traversal still walks through
		// non-matching exec neighbors. If you ask for entry_points anchored at a node,
		// you get the entry-points reachable through the traversal. offset does not apply
		// here (per the schema contract) and max_nodes already bounded the walk via BFSCap.
		if (!AnchorGuid.IsEmpty())
		{
			if (NodeFilter != TEXT("all") || bHasExtraFilters)
			{
				NodesToProcess = NodesToProcess.FilterByPredicate([&PassesNodeFilter, &PassesExtraFilters](UEdGraphNode* N)
				{
					return PassesNodeFilter(N) && PassesExtraFilters(N);
				});
			}
			TotalFiltered = NodesToProcess.Num();
		}

		// Pure-subgraph expansion. Walk data-input pins on each selected node and add
		// any pure source nodes (UK2Node::IsNodePure() == true) feeding them. Repeat to a
		// small depth so transitive pure chains are surfaced (e.g. promotable +
		// SelectFloat -> consumer). Cap iterations to avoid pathological math graphs.
		if (bIncludePureSubgraph)
		{
			TSet<UEdGraphNode*> Selected;
			for (UEdGraphNode* N : NodesToProcess) { Selected.Add(N); }

			const int32 MaxPureDepth = 6;
			for (int32 Depth = 0; Depth < MaxPureDepth; ++Depth)
			{
				bool bAddedAny = false;
				TArray<UEdGraphNode*> Snapshot = NodesToProcess;
				for (UEdGraphNode* Node : Snapshot)
				{
					if (!IsValid(Node)) { continue; }
					for (UEdGraphPin* Pin : Node->Pins)
					{
						if (!Pin || Pin->Direction != EGPD_Input) { continue; }
						if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec) { continue; }
						for (UEdGraphPin* Linked : Pin->LinkedTo)
						{
							if (!Linked) { continue; }
							UEdGraphNode* Src = Linked->GetOwningNode();
							if (!IsValid(Src) || Selected.Contains(Src)) { continue; }
							// Only pull in pure nodes -- avoid accidentally inflating result with exec-mode peers.
							UK2Node* K2Src = Cast<UK2Node>(Src);
							if (!IsValid(K2Src) || !K2Src->IsNodePure()) { continue; }
							Selected.Add(Src);
							NodesToProcess.Add(Src);
							bAddedAny = true;
						}
					}
				}
				if (!bAddedAny) { break; }
			}
		}

		for (UEdGraphNode* Node : NodesToProcess)
		{
			if (!IsValid(Node))
			{
				continue;
			}

			TSharedPtr<FJsonObject> NodeObj = MakeShared<FJsonObject>();
			if (!Node->NodeGuid.IsValid())
			{
				Node->CreateNewGuid();
				UE_LOG(LogClaireon, Warning,
					TEXT("[get_graph] Node '%s' in graph '%s' had invalid GUID; assigned %s."),
					*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					*Graph->GetName(),
					*Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			}
			NodeObj->SetStringField(TEXT("node_id"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			const FString ClassName = Node->GetClass()->GetName();
			const FString AliasName = ClaireonBlueprintHelpers::GetNodeTypeAliasForClass(Node->GetClass());
			// keep prefixed form only.
			NodeObj->SetStringField(TEXT("node_class"), ClassName);
			if (!AliasName.IsEmpty())
			{
				NodeObj->SetStringField(TEXT("node_type_alias"), AliasName);
			}
			else
			{
				// Generic escape hatch: runner must pass both fields back through add_node.
				NodeObj->SetStringField(TEXT("node_type_alias"), TEXT("Generic"));
				NodeObj->SetStringField(TEXT("generic_class_name"), ClassName);
			}
			const FString NodeTitleStr = GetNodeTitle(Node);
			// keep prefixed form only.
			NodeObj->SetStringField(TEXT("node_title"), NodeTitleStr);

			// Full detail: emit node_subtitle when the node's FullTitle has a second line
			// (e.g. "Target is Kismet System Library" on K2Node_CallFunction nodes).
			if (DetailLevel == TEXT("full"))
			{
				const FString FullTitleStr = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
				TArray<FString> SubtitleLines;
				FullTitleStr.ParseIntoArray(SubtitleLines, TEXT("\n"), true);
				if (SubtitleLines.Num() > 1)
				{
					FString SubtitleValue = SubtitleLines[1];
					SubtitleValue.TrimStartAndEndInline();
					if (!SubtitleValue.IsEmpty())
					{
						NodeObj->SetStringField(TEXT("node_subtitle"), SubtitleValue);
					}
				}
			}

			TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
			PosObj->SetNumberField(TEXT("x"), Node->NodePosX);
			PosObj->SetNumberField(TEXT("y"), Node->NodePosY);
			NodeObj->SetObjectField(TEXT("position"), PosObj);

			// Member references (function/variable/macro/cast/proxy/delegate/...)
			// carry the node's replayable identity; without them callers had to
			// regex-harvest T3D exports. Emitted at 'full' and 'summary' detail.
			if (DetailLevel == TEXT("full") || DetailLevel == TEXT("summary"))
			{
				ClaireonBlueprintNodeSerializer::AppendMemberReferenceFields(Node, NodeObj);
			}

			// linked_to is emitted at 'full' and 'exec' detail so pin-level payloads carry
			// actual endpoints (node GUID + pin name), not just a connection_count.
			const bool bEmitLinkedTo = (DetailLevel == TEXT("full") || DetailLevel == TEXT("exec"));

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

				// Per-pin endpoints: node_id (DigitsWithHyphens, string-matching nodes[].node_id)
				// plus the far pin's name. Without this, connections were only recoverable from
				// the graph-level connections[] array, which drops nothing now but is edge-form.
				if (bEmitLinkedTo && Pin->LinkedTo.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> LinkedToArray;
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						if (!LinkedPin)
						{
							continue;
						}
						UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
						if (!IsValid(LinkedNode))
						{
							continue;
						}
						TSharedPtr<FJsonObject> LinkObj = MakeShared<FJsonObject>();
						LinkObj->SetStringField(TEXT("node_id"), LinkedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						LinkObj->SetStringField(TEXT("pin_name"), LinkedPin->PinName.ToString());
						LinkedToArray.Add(MakeShared<FJsonValueObject>(LinkObj));
					}
					PinObj->SetArrayField(TEXT("linked_to"), LinkedToArray);
				}

				// surface default_value on unconnected pins at both 'full' and 'summary'
				// detail levels so callers can tell how a node will behave without escalating
				// to blueprint_graph_inspect_node (PIE-blocked). 'exec' / 'outline' remain
				// compact and omit defaults. Suppressed entirely by include_pin_defaults=false.
				if (bIncludePinDefaults
					&& (DetailLevel == TEXT("full") || DetailLevel == TEXT("summary"))
					&& !Pin->DefaultValue.IsEmpty()
					&& Pin->LinkedTo.Num() == 0)
				{
					PinObj->SetStringField(TEXT("default_value"), Pin->DefaultValue);
				}

				// Object/class pin identities live in DefaultObject, not DefaultValue
				// (e.g. CreateWidget's Class pin, asset-reference pins). Emit the object
				// path so replay can restore it via bp_set_pin_value.
				if (bIncludePinDefaults
					&& (DetailLevel == TEXT("full") || DetailLevel == TEXT("summary"))
					&& Pin->DefaultObject
					&& Pin->LinkedTo.Num() == 0)
				{
					PinObj->SetStringField(TEXT("default_object"), Pin->DefaultObject->GetPathName());
				}

				// split parent pins (DestLocation -> _X/_Y/_Z) carry their actual
				// connections on the sub-pins, not the parent. At 'full' detail, attach a
				// sub_pins[] array on the parent so callers can see the tree structure
				// alongside the existing flat sub-pin entries in this same Pins array.
				if (DetailLevel == TEXT("full") && Pin->SubPins.Num() > 0)
				{
					TArray<TSharedPtr<FJsonValue>> SubPinsArray;
					for (UEdGraphPin* SubPin : Pin->SubPins)
					{
						if (!SubPin) { continue; }
						TSharedPtr<FJsonObject> SubObj = MakeShared<FJsonObject>();
						SubObj->SetStringField(TEXT("pin_name"), SubPin->PinName.ToString());
						SubObj->SetStringField(TEXT("pin_type"), GetPinTypeString(SubPin));
						SubObj->SetStringField(TEXT("direction"), SubPin->Direction == EGPD_Input ? TEXT("input") : TEXT("output"));
						SubObj->SetNumberField(TEXT("connection_count"), SubPin->LinkedTo.Num());
						if (SubPin->LinkedTo.Num() > 0)
						{
							TArray<TSharedPtr<FJsonValue>> SubLinkedToArray;
							for (UEdGraphPin* SubLinkedPin : SubPin->LinkedTo)
							{
								if (!SubLinkedPin) { continue; }
								UEdGraphNode* SubLinkedNode = SubLinkedPin->GetOwningNode();
								if (!IsValid(SubLinkedNode)) { continue; }
								TSharedPtr<FJsonObject> SubLinkObj = MakeShared<FJsonObject>();
								SubLinkObj->SetStringField(TEXT("node_id"), SubLinkedNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
								SubLinkObj->SetStringField(TEXT("pin_name"), SubLinkedPin->PinName.ToString());
								SubLinkedToArray.Add(MakeShared<FJsonValueObject>(SubLinkObj));
							}
							SubObj->SetArrayField(TEXT("linked_to"), SubLinkedToArray);
						}
						if (bIncludePinDefaults && !SubPin->DefaultValue.IsEmpty() && SubPin->LinkedTo.Num() == 0)
						{
							SubObj->SetStringField(TEXT("default_value"), SubPin->DefaultValue);
						}
						SubPinsArray.Add(MakeShared<FJsonValueObject>(SubObj));
					}
					PinObj->SetArrayField(TEXT("sub_pins"), SubPinsArray);
				}

				PinsArray.Add(MakeShared<FJsonValueObject>(PinObj));
			}
			NodeObj->SetArrayField(TEXT("pins"), PinsArray);

			NodesArray.Add(MakeShared<FJsonValueObject>(NodeObj));
		}

		// Build connections array.
		// GUID format contract: from_node/to_node use EGuidFormats::DigitsWithHyphens so
		// they string-match nodes[].node_id (the payload uses ONE GUID format throughout).
		// Truncation contract: edges touching nodes outside the returned node set are KEPT
		// and flagged with target_in_set:false / source_in_set:false instead of dropped,
		// so a capped read still exposes the surviving nodes' full connectivity.
		TArray<TSharedPtr<FJsonValue>> ConnectionsArray;
		TSet<UEdGraphNode*> ProcessedSet(NodesToProcess);
		for (UEdGraphNode* Node : NodesToProcess)
		{
			if (!IsValid(Node))
			{
				continue;
			}
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin)
				{
					continue;
				}

				if (Pin->Direction == EGPD_Output)
				{
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						UEdGraphNode* TargetNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
						if (!IsValid(TargetNode))
						{
							continue;
						}

						TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
						ConnObj->SetStringField(TEXT("from_node"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						ConnObj->SetStringField(TEXT("from_pin"), Pin->PinName.ToString());
						ConnObj->SetStringField(TEXT("to_node"), TargetNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						ConnObj->SetStringField(TEXT("to_pin"), LinkedPin->PinName.ToString());
						if (!ProcessedSet.Contains(TargetNode))
						{
							ConnObj->SetBoolField(TEXT("target_in_set"), false);
						}
						ConnectionsArray.Add(MakeShared<FJsonValueObject>(ConnObj));
					}
				}
				else if (Pin->Direction == EGPD_Input)
				{
					// Inbound edges from OUT-OF-SET sources. In-set sources are already
					// covered by the output-pin iteration above (avoids duplicates).
					for (UEdGraphPin* LinkedPin : Pin->LinkedTo)
					{
						UEdGraphNode* SourceNode = LinkedPin ? LinkedPin->GetOwningNode() : nullptr;
						if (!IsValid(SourceNode) || ProcessedSet.Contains(SourceNode))
						{
							continue;
						}

						TSharedPtr<FJsonObject> ConnObj = MakeShared<FJsonObject>();
						ConnObj->SetStringField(TEXT("from_node"), SourceNode->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						ConnObj->SetStringField(TEXT("from_pin"), LinkedPin->PinName.ToString());
						ConnObj->SetStringField(TEXT("to_node"), Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
						ConnObj->SetStringField(TEXT("to_pin"), Pin->PinName.ToString());
						ConnObj->SetBoolField(TEXT("source_in_set"), false);
						ConnectionsArray.Add(MakeShared<FJsonValueObject>(ConnObj));
					}
				}
			}
		}

		GraphObj->SetArrayField(TEXT("nodes"), NodesArray);
		GraphObj->SetNumberField(TEXT("node_count"), NodesArray.Num());
		GraphObj->SetArrayField(TEXT("connections"), ConnectionsArray);
		GraphObj->SetNumberField(TEXT("connection_count"), ConnectionsArray.Num());
		GraphObj->SetNumberField(TEXT("total_nodes_in_graph"), Graph->Nodes.Num());
		GraphObj->SetNumberField(TEXT("total_filtered"), TotalFiltered);

		if (bIncludeT3D)
		{
			GraphObj->SetStringField(TEXT("t3d"), BuildGraphT3DExport(Graph));
		}

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

	FToolResult Result = MakeSuccessResult(Data, Summary);

	// Success-path guidance. The reported friction was a caller concluding that connectivity
	// was unavailable when it was simply not emitted at the detail level they asked for --
	// nothing in the payload said so, and the field they guessed at ('links') never existed.
	// This makes the omission self-describing.
	//
	// Mirrors the emission conditions above exactly: linked_to at 'full'/'exec' (line ~740),
	// pin defaults at 'full'/'summary' gated on include_pin_defaults (~786).
	{
		const bool bConnectivityOmitted = !(DetailLevel == TEXT("full") || DetailLevel == TEXT("exec"));
		const bool bPinDefaultsOmitted =
			!(bIncludePinDefaults && (DetailLevel == TEXT("full") || DetailLevel == TEXT("summary")));

		// LATCHED per session by code: this teaches a PARAMETER, and that lesson transfers the
		// moment it lands, so repeating it on every call of a sweep is the bulk-loop noise that
		// forced a latch onto the get_editor_property nudge.
		if ((bConnectivityOmitted || bPinDefaultsOmitted)
			&& ShouldEmitLatchedHint(TEXT("bp_get_graph_detail_omissions")))
		{
			// ONE hint, not two. Both omissions share a single remedy -- one corrected re-call
			// -- and two reasons with one action is one hint. The reason enumerates each
			// omission with its OWN flag so a caller who only wants connectivity can take half
			// the advice, rather than maxing every detail on a large graph and spilling through
			// the Output Gate.
			TArray<FString> Omissions;
			if (bConnectivityOmitted)
			{
				Omissions.Add(FString::Printf(
					TEXT("per-pin connectivity (pins[].linked_to) is not emitted at node_detail_level='%s'; ")
					TEXT("use 'full' or 'exec' for it. The graph-level connections[] array is always present ")
					TEXT("and never drops edges"),
					*DetailLevel));
			}
			if (bPinDefaultsOmitted)
			{
				Omissions.Add(FString::Printf(
					TEXT("pin default values are not emitted%s at node_detail_level='%s'; ")
					TEXT("use 'full' or 'summary' with include_pin_defaults=true"),
					bIncludePinDefaults ? TEXT("") : TEXT(" when include_pin_defaults=false"),
					*DetailLevel));
			}

			// args echoes the original call with every correction applied, so it stays directly
			// callable -- a bare {node_detail_level} delta would re-issue without asset_path.
			TSharedPtr<FJsonObject> FullArgs = CloneHintArgs(Arguments);
			FullArgs->SetStringField(TEXT("node_detail_level"), TEXT("full"));
			FullArgs->SetBoolField(TEXT("include_pin_defaults"), true);

			Result.Hint = MakeGuidanceHint(GetName(), FString::Join(Omissions, TEXT(". ")), FullArgs);
		}
	}
	return Result;
}

UBlueprint* ClaireonTool_GetBlueprintGraph::LoadBlueprintFromPath(const FString& AssetPath, FString& OutError)
{
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!IsValid(Blueprint))
	{
		OutError = FString::Printf(TEXT("Failed to load Blueprint at path: %s. Ensure the path is correct and the asset is a Blueprint."), *AssetPath);
		return nullptr;
	}

	return Blueprint;
}


UEdGraph* ClaireonTool_GetBlueprintGraph::FindGraphByName(const UBlueprint* Blueprint, const FString& GraphName, FString& OutError)
{
	// Search in UbergraphPages (event graphs)
	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (IsValid(Graph) && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Search in FunctionGraphs
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (IsValid(Graph) && Graph->GetName() == GraphName)
		{
			return Graph;
		}
	}

	// Everything else, including macro/delegate graphs and composite
	// (collapsed-graph) subgraphs, which live recursively in SubGraphs.
	{
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (IsValid(Graph) && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}
	}

	OutError = FString::Printf(TEXT("Graph '%s' not found in Blueprint. Use bp_get_properties to see available graphs."), *GraphName);
	return nullptr;
}

FString ClaireonTool_GetBlueprintGraph::BuildGraphJsonSummary(const UEdGraph* Graph, const FString& DetailLevel, int32 MaxNodes, const FString& AnchorGuid, int32 TraversalDepth)
{
	if (!IsValid(Graph))
	{
		return TEXT("Error: Invalid graph");
	}

	// AnimBP graphs don't use PC_Exec pins - fall back to "summary" detail
	FString EffectiveDetailLevel = DetailLevel;
	FString AnimBPFallbackNote;
	if (DetailLevel == TEXT("exec") && IsValid(Graph->Schema) && Graph->Schema->IsChildOf(UAnimationGraphSchema::StaticClass()))
	{
		EffectiveDetailLevel = TEXT("summary");
		AnimBPFallbackNote = TEXT("\n(exec detail not available for AnimBP graphs - showing summary)");
	}

	FString Output;
	Output += FString::Printf(TEXT("## Graph: %s (%d nodes)\n\n"), *Graph->GetName(), Graph->Nodes.Num());
	Output += TEXT("### Nodes\n\n");

	// --- Anchor BFS path ---
	if (!AnchorGuid.IsEmpty())
	{
		// Find the anchor node via the shared >=8-hex prefix resolver (full GUID or
		// unique prefix). Unresolvable/ambiguous/malformed -> structured error text.
		UEdGraphNode* AnchorNode = nullptr;
		FString AnchorError;
		if (!ClaireonBlueprintHelpers::ResolveNodeGuidString(Graph, AnchorGuid, AnchorNode, AnchorError, TEXT("anchor_node_guid")))
		{
			return AnchorError;
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
					if (!IsValid(Neighbor) || Visited.Contains(Neighbor->NodeGuid))
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
						if (!IsValid(Neighbor) || DataNeighborVisited.Contains(Neighbor->NodeGuid))
						{
							continue;
						}
						DataNeighborVisited.Add(Neighbor->NodeGuid);
						Output += FString::Printf(TEXT("%d. %s\n"), NeighborIdx++, *FormatNodeSummary(Neighbor, TEXT("outline")));
					}
				}

				FString AnchorTitle = GetNodeTitle(AnchorNode);
				Output += FString::Printf(TEXT("\n(Anchor node [%s] has no exec connections - showing data-pin neighbors instead.)"), *AnchorTitle);
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
			TEXT("\n(Showing %d of %d nodes - anchored at [%s], depth %s.\n %d nodes not shown. Use anchor_node_guid=<guid> to navigate to a different section.)"),
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
		if (!IsValid(Node))
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
	if (!IsValid(Graph))
	{
		return TEXT("Error: Invalid graph");
	}

	// Collect all nodes to export
	TSet<UObject*> NodesToExport;
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (IsValid(Node))
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
	if (!IsValid(Node))
	{
		return TEXT("[Invalid Node]");
	}

	// Split FullTitle into clean first line and optional subtitle ("Target is X").
	const FString FullTitle = Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	TArray<FString> TitleLines;
	FullTitle.ParseIntoArray(TitleLines, TEXT("\n"), true);
	FString CleanTitle = TitleLines.Num() > 0 ? TitleLines[0].TrimStartAndEnd() : FullTitle;
	const FString Subtitle = TitleLines.Num() > 1 ? TitleLines[1].TrimStartAndEnd() : FString();

	FString NodeClass = Node->GetClass()->GetName();
	FVector2D NodePos(Node->NodePosX, Node->NodePosY);

	// Outline mode: single-line grammar per FRACTURE/04_outline_format.md.
	// "<class> <guid8>  <clean_title>  @ (x, y)" — no embedded newlines. The
	// caller (BuildGraphJsonSummary) prepends the "<index>. " prefix.
	if (DetailLevel == TEXT("outline"))
	{
		const FString ShortGuid = Node->NodeGuid.ToString(EGuidFormats::Digits).Left(8).ToLower();
		FString ListTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
		ListTitle = ListTitle.Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\r"), TEXT(" "));
		ListTitle.TrimStartAndEndInline();
		return FString::Printf(
			TEXT("%s %s  %s  @ (%.0f, %.0f)"),
			*NodeClass, *ShortGuid, *ListTitle, NodePos.X, NodePos.Y);
	}

	// Non-outline modes keep today's bracketed header using the clean (first-line) title.
	FString Summary = FString::Printf(TEXT("[%s] (%s) @ (%.0f, %.0f)"),
		*CleanTitle, *NodeClass, NodePos.X, NodePos.Y);

	// Add GUID for reference
	if (DetailLevel == TEXT("full") || DetailLevel == TEXT("summary") || DetailLevel == TEXT("exec"))
	{
		Summary += FString::Printf(TEXT(" [GUID: %s]"), *Node->NodeGuid.ToString());
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
						if (LinkedPin && IsValid(LinkedPin->GetOwningNode()))
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

	// Full-only: render "Target is X" on its own indented line (no raw \n in the title).
	if (!Subtitle.IsEmpty() && DetailLevel == TEXT("full"))
	{
		Summary += FString::Printf(TEXT("\n   Target: %s"), *Subtitle);
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
			if (LinkedPin && IsValid(LinkedPin->GetOwningNode()))
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
		if (!IsValid(Node))
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
	if (!IsValid(Node))
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
