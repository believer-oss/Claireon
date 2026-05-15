// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_MoveNode.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_MoveNode::GetName() const
{
	return TEXT("claireon.behaviortree_move_node");
}

FString ClaireonBehaviorTreeTool_MoveNode::GetDescription() const
{
	return TEXT("Move a node under a different parent. Disconnects from old parent and connects to new parent at child_index.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_MoveNode::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the node to move."), true);
	Builder.AddString(TEXT("new_parent_id"), TEXT("GUID of the new parent (or the root node)."), true);
	Builder.AddInteger(TEXT("child_index"), TEXT("Optional zero-based insertion index on the new parent. -1 (default) appends."));
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_MoveNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	FGuid NewParentGuid;
	if (!ParseGuidParam(Arguments, TEXT("node_id"), NodeGuid, Error))
	{
		return MakeErrorResult(Error);
	}
	if (!ParseGuidParam(Arguments, TEXT("new_parent_id"), NewParentGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	int32 ChildIndex = -1;
	Arguments->TryGetNumberField(TEXT("child_index"), ChildIndex);

	UBehaviorTreeGraphNode* GraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!GraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	UBehaviorTreeGraphNode* NewParentNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NewParentGuid);
	if (!NewParentNode)
	{
		UBehaviorTreeGraphNode_Root* RootNode = ClaireonBehaviorTreeHelpers::FindRootGraphNode(Graph);
		if (RootNode && RootNode->NodeGuid == NewParentGuid)
		{
			NewParentNode = RootNode;
		}
		else
		{
			return MakeErrorResult(FString::Printf(TEXT("New parent node not found: %s"), *NewParentGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Move BT Node")));
	Data->BehaviorTree->Modify();

	ClaireonBehaviorTreeHelpers::DisconnectNode(GraphNode, Error);

	if (!ClaireonBehaviorTreeHelpers::ConnectNodes(NewParentNode, GraphNode, ChildIndex, Error))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to connect to new parent: %s"), *Error));
	}

	Data->FocusedNodeGuid = NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("move_node - Moved {%s} under {%s}"),
		*NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
		*NewParentGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	return BuildStateResponse(SessionId, Data);
}
