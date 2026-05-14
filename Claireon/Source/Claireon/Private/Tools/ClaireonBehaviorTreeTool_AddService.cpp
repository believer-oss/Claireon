// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_AddService.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonNameResolver.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BTService.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_AddService::GetOperation() const { return TEXT("add_service"); }

FString ClaireonBehaviorTreeTool_AddService::GetDescription() const
{
	return TEXT("Add a service subnode to an existing composite or task node.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_AddService::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the parent node to attach the service to."), true);
	Builder.AddString(TEXT("service_class"), TEXT("BTService subclass name (e.g. BTService_DefaultFocus)."), true);
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_AddService::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString ServiceClassName;
	if (!Arguments->TryGetStringField(TEXT("service_class"), ServiceClassName) || ServiceClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: service_class"));
	}

	UBehaviorTreeGraphNode* ParentGraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!ParentGraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	ClaireonNameResolver::FNameResolveResult ServiceNameResult;
	UClass* ServiceClass = ClaireonNameResolver::ResolveClassName(ServiceClassName, UBTService::StaticClass(), ServiceNameResult);
	if (!ServiceClass)
	{
		return MakeErrorResult(ServiceNameResult.Error);
	}

	if (!ServiceClass->IsChildOf(UBTService::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Class '%s' is not a BTService subclass"), *ServiceClassName));
	}

	UBehaviorTreeGraphNode* ServiceGraphNode = ClaireonBehaviorTreeHelpers::CreateGraphNodeForClass(Graph, ServiceClass,
		FVector2D(ParentGraphNode->NodePosX, ParentGraphNode->NodePosY + 50), Error);
	if (!ServiceGraphNode)
	{
		return MakeErrorResult(Error);
	}

	// AddSubNode manages its own FScopedTransaction
	ParentGraphNode->AddSubNode(ServiceGraphNode, Graph);

	Data->FocusedNodeGuid = ServiceGraphNode->NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("add_service - Added %s to {%s}"),
		*ServiceClassName, *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));
	if (!ServiceNameResult.ResolutionNote.IsEmpty())
	{
		Data->LastOperationStatus += FString::Printf(TEXT(" [note: %s]"), *ServiceNameResult.ResolutionNote);
	}

	return BuildStateResponse(SessionId, Data);
}
