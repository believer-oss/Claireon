// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_RemoveService.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "AIGraphNode.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_RemoveService::GetOperation() const { return TEXT("remove_service"); }

FString ClaireonBehaviorTreeTool_RemoveService::GetDescription() const
{
	return TEXT("Remove a service subnode from its parent composite or task node within an open "
				"Behavior Tree session. Requires session_id from behavior_tree.open; the edit is "
				"transactional and only persists after save.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_RemoveService::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the parent node."), true);
	Builder.AddString(TEXT("service_id"), TEXT("GUID of the service subnode to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_RemoveService::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FGuid ServiceGuid;
	if (!ParseGuidParam(Arguments, TEXT("service_id"), ServiceGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	UBehaviorTreeGraphNode* ParentGraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!ParentGraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	UAIGraphNode* ServiceSubNode = nullptr;
	for (UAIGraphNode* SubNode : ParentGraphNode->SubNodes)
	{
		UBehaviorTreeGraphNode* SubBTNode = Cast<UBehaviorTreeGraphNode>(SubNode);
		if (SubBTNode && SubBTNode->NodeGuid == ServiceGuid)
		{
			ServiceSubNode = SubNode;
			break;
		}
	}

	if (!ServiceSubNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Service not found with GUID: %s"), *ServiceGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	// RemoveSubNode manages its own FScopedTransaction
	ParentGraphNode->RemoveSubNode(ServiceSubNode);

	Data->FocusedNodeGuid = NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("remove_service - Removed {%s} from {%s}"),
		*ServiceGuid.ToString(EGuidFormats::DigitsWithHyphensLower),
		*NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	return BuildStateResponse(SessionId, Data);
}
