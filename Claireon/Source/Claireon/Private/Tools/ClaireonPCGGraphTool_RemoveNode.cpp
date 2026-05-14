// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphTool_RemoveNode.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonPCGGraphTool_RemoveNode::GetOperation() const { return TEXT("remove_node"); }

FString ClaireonPCGGraphTool_RemoveNode::GetDescription() const
{
	return TEXT("Remove a node from the PCG graph by index or name. Input and Output nodes cannot be removed.");
}

TSharedPtr<FJsonObject> ClaireonPCGGraphTool_RemoveNode::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node"), TEXT("Node identifier (index or name) to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonPCGGraphTool_RemoveNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FPCGGraphEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString NodeIdentifier;
	if (!Arguments->TryGetStringField(TEXT("node"), NodeIdentifier) || NodeIdentifier.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: node"));
	}

	int32 NodeIndex;
	UPCGNode* Node = ClaireonPCGGraphHelpers::FindNodeByIdentifier(Data->PCGGraph.Get(), NodeIdentifier, NodeIndex);
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeIdentifier));
	}

	// Don't allow removing input/output nodes
	if (Node == Data->PCGGraph->GetInputNode() || Node == Data->PCGGraph->GetOutputNode())
	{
		return MakeErrorResult(TEXT("Cannot remove the graph's built-in Input or Output node"));
	}

	FString RemovedName = ClaireonPCGGraphHelpers::GetNodeDisplayName(Node);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove PCG Node")));
	Data->PCGGraph->RemoveNode(Node);
	ClaireonPCGGraphHelpers::NotifyGraphChanged(Data->PCGGraph.Get());

	Data->LastOperationStatus = FString::Printf(TEXT("Removed node: %s"), *RemovedName);

	// Reset cursor if it pointed to the removed node
	if (Data->FocusedNodeIndex == NodeIndex)
	{
		Data->FocusedNodeIndex = INDEX_NONE;
	}
	else if (Data->FocusedNodeIndex > NodeIndex && Data->FocusedNodeIndex != INDEX_NONE)
	{
		Data->FocusedNodeIndex--;
	}

	return BuildStateResponse(SessionId, Data);
}
