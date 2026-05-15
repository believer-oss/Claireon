// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_RemoveDecorator.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "AIGraphNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_RemoveDecorator::GetName() const
{
	return TEXT("claireon.behaviortree_remove_decorator");
}

FString ClaireonBehaviorTreeTool_RemoveDecorator::GetDescription() const
{
	return TEXT("Remove a decorator subnode from its parent node.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_RemoveDecorator::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the parent node."), true);
	Builder.AddString(TEXT("decorator_id"), TEXT("GUID of the decorator subnode to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_RemoveDecorator::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FGuid DecoratorGuid;
	if (!ParseGuidParam(Arguments, TEXT("decorator_id"), DecoratorGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	UBehaviorTreeGraphNode* ParentGraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!ParentGraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	UAIGraphNode* DecoratorSubNode = nullptr;
	for (UAIGraphNode* SubNode : ParentGraphNode->SubNodes)
	{
		UBehaviorTreeGraphNode* SubBTNode = Cast<UBehaviorTreeGraphNode>(SubNode);
		if (SubBTNode && SubBTNode->NodeGuid == DecoratorGuid)
		{
			DecoratorSubNode = SubNode;
			break;
		}
	}

	if (!DecoratorSubNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Decorator not found with GUID: %s"), *DecoratorGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	// RemoveSubNode manages its own FScopedTransaction
	ParentGraphNode->RemoveSubNode(DecoratorSubNode);

	Data->FocusedNodeGuid = NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("remove_decorator - Removed {%s} from {%s}"),
		*DecoratorGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
		*NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	return BuildStateResponse(SessionId, Data);
}
