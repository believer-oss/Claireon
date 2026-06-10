// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_AddDecorator.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonNameResolver.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTDecorator.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_AddDecorator::GetOperation() const { return TEXT("add_decorator"); }

FString ClaireonBehaviorTreeTool_AddDecorator::GetDescription() const
{
	return TEXT("Add a decorator subnode to an existing composite or task node within an open Behavior "
				"Tree session. Requires session_id from behavior_tree.open; the edit is transactional "
				"and only persists after save. Use list_node_types with category=decorator to discover "
				"available decorator classes.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_AddDecorator::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the parent node to attach the decorator to."), true);
	Builder.AddString(TEXT("decorator_class"), TEXT("BTDecorator subclass name (e.g. BTDecorator_Blackboard)."), true);
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_AddDecorator::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString DecoratorClassName;
	if (!Arguments->TryGetStringField(TEXT("decorator_class"), DecoratorClassName) || DecoratorClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: decorator_class"));
	}

	UBehaviorTreeGraphNode* ParentGraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!ParentGraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	ClaireonNameResolver::FNameResolveResult DecoratorNameResult;
	UClass* DecoratorClass = ClaireonNameResolver::ResolveClassName(DecoratorClassName, UBTDecorator::StaticClass(), DecoratorNameResult);
	if (!DecoratorClass)
	{
		return MakeErrorResult(DecoratorNameResult.Error);
	}

	if (!DecoratorClass->IsChildOf(UBTDecorator::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Class '%s' is not a BTDecorator subclass"), *DecoratorClassName));
	}

	UBehaviorTreeGraphNode* DecoratorGraphNode = ClaireonBehaviorTreeHelpers::CreateGraphNodeForClass(Graph, DecoratorClass,
		FVector2D(ParentGraphNode->NodePosX, ParentGraphNode->NodePosY - 50), Error);
	if (!DecoratorGraphNode)
	{
		return MakeErrorResult(Error);
	}

	// AddSubNode manages its own FScopedTransaction
	ParentGraphNode->AddSubNode(DecoratorGraphNode, Graph);

	Data->FocusedNodeGuid = DecoratorGraphNode->NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("add_decorator - Added %s to {%s}"),
		*DecoratorClassName, *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	if (!DecoratorNameResult.ResolutionNote.IsEmpty())
	{
		Data->LastOperationStatus += FString::Printf(TEXT(" [note: %s]"), *DecoratorNameResult.ResolutionNote);
	}

	return BuildStateResponse(SessionId, Data);
}
