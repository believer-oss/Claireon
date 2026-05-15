// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_InspectBlueprintNode.h"

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonBlueprintNodeSerializer.h"
#include "AnimGraphNode_Base.h"
#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"

FString ClaireonTool_InspectBlueprintNode::GetName() const
{
	return TEXT("claireon.blueprint_inspect_node");
}

FString ClaireonTool_InspectBlueprintNode::GetDescription() const
{
	return TEXT("Returns a single Blueprint node in full fidelity as structured JSON. "
		"Requires asset_path, graph_name, and node_guid. AnimGraph nodes are redirected "
		"to claireon.animgraph_get_node.");
}

FString ClaireonTool_InspectBlueprintNode::GetFullDescription() const
{
	return TEXT(
		"claireon.blueprint_inspect_node - Stateless read of a single Blueprint node.\n"
		"\n"
		"Required arguments:\n"
		"  asset_path (string): full /Game/... path to a Blueprint.\n"
		"  graph_name (string): exact graph name (node GUIDs are only unique within a graph).\n"
		"  node_guid  (string): any FGuid::Parse-compatible format.\n"
		"\n"
		"Optional arguments:\n"
		"  include_connections  (bool, default true): emit linked_to arrays on pins.\n"
		"  include_pin_defaults (bool, default true): emit default_value/_object/_text fields.\n"
		"\n"
		"Payload shape: JSON object with node_id, node_class, node_title, node_subtitle (when\n"
		"the title has a second line), position { x, y }, per-class fields (function_reference,\n"
		"variable_reference, macro_reference, custom_event_name, target_class), and an ordered\n"
		"pins array. Each pin has pin_id, pin_name, direction, structured pin_type, linked_count,\n"
		"and optional linked_to / default_* fields.\n"
		"\n"
		"Truncation: linked_to is capped at 32 entries per pin (linked_to_truncated/linked_to_total\n"
		"flags surface when exceeded). default_value strings are capped at 1024 bytes\n"
		"(default_value_truncated flag when exceeded).\n"
		"\n"
		"AnimGraph redirect: nodes derived from UAnimGraphNode_Base return an error pointing at\n"
		"claireon.animgraph_get_node rather than a degraded generic payload.\n"
		"\n"
		"Use this tool when you already know the asset_path + graph_name + node_guid. For cursor-\n"
		"based navigation, use claireon.blueprint_edit_graph with operation='inspect_node' instead.");
}

TSharedPtr<FJsonObject> ClaireonTool_InspectBlueprintNode::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPath = MakeShared<FJsonObject>();
	AssetPath->SetStringField(TEXT("type"), TEXT("string"));
	AssetPath->SetStringField(TEXT("description"), TEXT("Full /Game/... path to the Blueprint asset to inspect."));
	Properties->SetObjectField(TEXT("asset_path"), AssetPath);

	TSharedPtr<FJsonObject> GraphName = MakeShared<FJsonObject>();
	GraphName->SetStringField(TEXT("type"), TEXT("string"));
	GraphName->SetStringField(TEXT("description"), TEXT("Name of the graph within the Blueprint (node GUIDs are only unique within a graph)."));
	Properties->SetObjectField(TEXT("graph_name"), GraphName);

	TSharedPtr<FJsonObject> NodeGuid = MakeShared<FJsonObject>();
	NodeGuid->SetStringField(TEXT("type"), TEXT("string"));
	NodeGuid->SetStringField(TEXT("description"), TEXT("GUID of the node to inspect; any FGuid::Parse-compatible format."));
	Properties->SetObjectField(TEXT("node_guid"), NodeGuid);

	TSharedPtr<FJsonObject> IncludeConnections = MakeShared<FJsonObject>();
	IncludeConnections->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeConnections->SetStringField(TEXT("description"), TEXT("If true (default), emit linked_to arrays on each pin."));
	Properties->SetObjectField(TEXT("include_connections"), IncludeConnections);

	TSharedPtr<FJsonObject> IncludePinDefaults = MakeShared<FJsonObject>();
	IncludePinDefaults->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludePinDefaults->SetStringField(TEXT("description"), TEXT("If true (default), emit default_value / default_object / default_text on pins."));
	Properties->SetObjectField(TEXT("include_pin_defaults"), IncludePinDefaults);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("graph_name")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("node_guid")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_InspectBlueprintNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString GraphName;
	if (!Arguments->TryGetStringField(TEXT("graph_name"), GraphName))
	{
		return MakeErrorResult(TEXT("Missing required field: graph_name"));
	}

	FString NodeGuidStr;
	if (!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));
	}

	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));
	}

	UEdGraph* Graph = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return MakeErrorResult(FString::Printf(TEXT("Graph '%s' not found"), *GraphName));
	}

	UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, NodeGuid);
	if (!Node)
	{
		const FString Available = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(
			TEXT("Node %s not found in graph '%s'. Available nodes: %s"),
			*NodeGuidStr, *Graph->GetName(), *Available));
	}

	if (Cast<UAnimGraphNode_Base>(Node))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Node %s is an AnimGraph node; use claireon.animgraph_get_node to inspect AnimGraph nodes."),
			*Node->NodeGuid.ToString(EGuidFormats::Digits)));
	}

	bool bIncludeConnections = true;
	Arguments->TryGetBoolField(TEXT("include_connections"), bIncludeConnections);
	bool bIncludePinDefaults = true;
	Arguments->TryGetBoolField(TEXT("include_pin_defaults"), bIncludePinDefaults);

	const FString Payload = ClaireonBlueprintNodeSerializer::SerializeNodeToString(
		Node, bIncludeConnections, bIncludePinDefaults);

	return MakeSuccessResult(nullptr, Payload);
}
