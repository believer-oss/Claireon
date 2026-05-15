// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_RemoveNode.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "EdGraph/EdGraphPin.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_RemoveNode::GetName() const
{
	return TEXT("claireon.behaviortree_remove_node");
}

FString ClaireonBehaviorTreeTool_RemoveNode::GetDescription() const
{
	return TEXT("Remove a node (composite or task) from the Behavior Tree. The root node cannot be removed.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_RemoveNode::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the node to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_RemoveNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FBehaviorTreeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(Error);
	}

	FGuid NodeGuid;
	if (!ParseGuidParam(Arguments, TEXT("node_id"), NodeGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	UBehaviorTreeGraphNode* GraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!GraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	if (Cast<UBehaviorTreeGraphNode_Root>(GraphNode))
	{
		return MakeErrorResult(TEXT("Cannot remove the root node"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove BT Node")));
	Data->BehaviorTree->Modify();

	ClaireonBehaviorTreeHelpers::DisconnectNode(GraphNode, Error);

	for (UEdGraphPin* Pin : GraphNode->Pins)
	{
		if (Pin)
		{
			Pin->BreakAllPinLinks();
		}
	}

	Graph->RemoveNode(GraphNode);

	Data->FocusedNodeGuid = FGuid();
	Data->LastOperationStatus = FString::Printf(TEXT("remove_node - Removed {%s}"),
		*NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	return BuildStateResponse(SessionId, Data);
}
