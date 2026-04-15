// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AnimGraphGetNode.h"
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonBlueprintHelpers.h"
#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_Base.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_AnimGraphGetNode::GetName() const
{
	return TEXT("claireon.animgraph_get_node");
}

FString ClaireonTool_AnimGraphGetNode::GetDescription() const
{
	return TEXT("Deep inspection of a single animation graph node by GUID. Returns ALL runtime FAnimNode "
		"properties, all pins with full connection details, property bindings with fast path analysis, "
		"linked layer interface info, bound event functions, editor properties, and sub-graph references.");
}

TSharedPtr<FJsonObject> ClaireonTool_AnimGraphGetNode::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the Animation Blueprint asset"), true);
	S.AddString(TEXT("graph_name"), TEXT("Name of the graph containing the node"), true);
	S.AddString(TEXT("node_guid"), TEXT("GUID of the node to inspect"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_AnimGraphGetNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString NodeGuidStr;
	if (!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidStr) || NodeGuidStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: node_guid"));
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

	// Find node by GUID
	FGuid ParsedGuid;
	FGuid::Parse(NodeGuidStr, ParsedGuid);
	UEdGraphNode* Node = ParsedGuid.IsValid()
		? ClaireonBlueprintHelpers::FindNodeByGuid(Graph, ParsedGuid)
		: nullptr;

	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node with GUID '%s' not found in graph '%s'"), *NodeGuidStr, *GraphName));
	}

	// Build comprehensive node data
	TSharedPtr<FJsonObject> Data = ClaireonAnimGraphHelpers::SerializeAnimGraphNode(Node, TEXT("full"), AnimBP);

	// Add runtime FAnimNode properties
	if (UAnimGraphNode_Base* AnimGraphNode = Cast<UAnimGraphNode_Base>(Node))
	{
		TSharedPtr<FJsonObject> RuntimeProps = ClaireonAnimGraphHelpers::SerializeAnimNodeProperties(AnimGraphNode);
		if (RuntimeProps)
		{
			Data->SetObjectField(TEXT("runtime_node_properties"), RuntimeProps);
		}

		// Also add editor-level UObject properties via ClaireonPropertyUtils
		TSharedPtr<FJsonObject> EditorProps = ClaireonPropertyUtils::GetAllProperties(AnimGraphNode, TEXT(""), 1);
		if (EditorProps)
		{
			Data->SetObjectField(TEXT("editor_properties"), EditorProps);
		}
	}

	FString Summary = FString::Printf(TEXT("Node '%s' (%s) in graph '%s'"),
		*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
		*ClaireonAnimGraphHelpers::GetAnimNodeCategory(Node),
		*GraphName);

	return MakeSuccessResult(Data, Summary);
}
