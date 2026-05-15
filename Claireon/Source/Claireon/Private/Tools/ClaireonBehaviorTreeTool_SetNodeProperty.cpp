// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_SetNodeProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTNode.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_SetNodeProperty::GetName() const
{
	return TEXT("claireon.behaviortree_set_node_property");
}

FString ClaireonBehaviorTreeTool_SetNodeProperty::GetDescription() const
{
	return TEXT("Set a property on a BT node instance via reflection (ImportText-based). Works on composites, tasks, decorators, and services.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_SetNodeProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the node whose property is being set."), true);
	Builder.AddString(TEXT("property_name"), TEXT("Name of the property on the node instance."), true);
	Builder.AddString(TEXT("property_value"), TEXT("Value to import (string form, parsed via FProperty::ImportText)."), true);
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_SetNodeProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_name"));
	}

	FString PropertyValue;
	if (!Arguments->TryGetStringField(TEXT("property_value"), PropertyValue))
	{
		return MakeErrorResult(TEXT("Missing required parameter: property_value"));
	}

	UBehaviorTreeGraphNode* GraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!GraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	UBTNode* NodeInstance = Cast<UBTNode>(GraphNode->NodeInstance);
	if (!NodeInstance)
	{
		return MakeErrorResult(TEXT("Node has no BT node instance"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set BT Node Property")));
	Data->BehaviorTree->Modify();

	if (!ClaireonBehaviorTreeHelpers::SetBTNodeProperty(NodeInstance, PropertyName, PropertyValue, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->FocusedNodeGuid = NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("set_node_property - Set '%s' = '%s' on {%s}"),
		*PropertyName, *PropertyValue, *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	return BuildStateResponse(SessionId, Data);
}
