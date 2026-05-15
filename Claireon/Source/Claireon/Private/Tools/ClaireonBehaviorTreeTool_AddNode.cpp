// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_AddNode.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonNameResolver.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTCompositeNode.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "BehaviorTreeGraphNode_Root.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_AddNode::GetName() const
{
	return TEXT("claireon.behaviortree_add_node");
}

FString ClaireonBehaviorTreeTool_AddNode::GetDescription() const
{
	return TEXT("Add a composite or task node as a child of an existing node. Use list_node_types to discover node classes.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_AddNode::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("parent_node_id"), TEXT("GUID of the parent node (or the root node)."), true);
	Builder.AddString(TEXT("node_class"), TEXT("BT node class name (e.g. BTComposite_Selector, BTTask_Wait)."), true);
	Builder.AddInteger(TEXT("child_index"), TEXT("Optional zero-based insertion index. -1 (default) appends."));
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_AddNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FGuid ParentGuid;
	if (!ParseGuidParam(Arguments, TEXT("parent_node_id"), ParentGuid, Error))
	{
		return MakeErrorResult(Error);
	}

	FString NodeClassName;
	if (!Arguments->TryGetStringField(TEXT("node_class"), NodeClassName) || NodeClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: node_class"));
	}

	int32 ChildIndex = -1;
	Arguments->TryGetNumberField(TEXT("child_index"), ChildIndex);

	ClaireonNameResolver::FNameResolveResult NameResult;
	UClass* NodeClass = ClaireonNameResolver::ResolveClassName(NodeClassName, UBTNode::StaticClass(), NameResult);
	if (!NodeClass)
	{
		return MakeErrorResult(NameResult.Error);
	}

	if (!NodeClass->IsChildOf(UBTCompositeNode::StaticClass()) && !NodeClass->IsChildOf(UBTTaskNode::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Class '%s' is not a composite or task node. Use add_decorator or add_service for those types."), *NodeClassName));
	}

	UBehaviorTreeGraphNode* ParentGraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, ParentGuid);
	if (!ParentGraphNode)
	{
		UBehaviorTreeGraphNode_Root* RootNode = ClaireonBehaviorTreeHelpers::FindRootGraphNode(Graph);
		if (RootNode && RootNode->NodeGuid == ParentGuid)
		{
			ParentGraphNode = RootNode;
		}
		else
		{
			return MakeErrorResult(FString::Printf(TEXT("Parent node not found with GUID: %s"), *ParentGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add BT Node")));
	Data->BehaviorTree->Modify();

	UBehaviorTreeGraphNode* NewNode = ClaireonBehaviorTreeHelpers::CreateGraphNodeForClass(Graph, NodeClass,
		FVector2D(ParentGraphNode->NodePosX + 200, ParentGraphNode->NodePosY + 100), Error);
	if (!NewNode)
	{
		return MakeErrorResult(Error);
	}

	if (!ClaireonBehaviorTreeHelpers::ConnectNodes(ParentGraphNode, NewNode, ChildIndex, Error))
	{
		return MakeErrorResult(FString::Printf(TEXT("Node created but failed to connect: %s"), *Error));
	}

	Data->FocusedNodeGuid = NewNode->NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("add_node - Added %s under parent {%s}"),
		*NodeClassName, *ParentGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	if (!NameResult.ResolutionNote.IsEmpty())
	{
		Data->LastOperationStatus += FString::Printf(TEXT(" [note: %s]"), *NameResult.ResolutionNote);
	}

	return BuildStateResponse(SessionId, Data);
}
