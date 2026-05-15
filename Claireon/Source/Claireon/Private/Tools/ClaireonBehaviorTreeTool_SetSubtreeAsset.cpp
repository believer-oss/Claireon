// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_SetSubtreeAsset.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/Tasks/BTTask_RunBehavior.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"
#include "ScopedTransaction.h"
#include "UObject/UnrealType.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_SetSubtreeAsset::GetName() const
{
	return TEXT("claireon.behaviortree_set_subtree_asset");
}

FString ClaireonBehaviorTreeTool_SetSubtreeAsset::GetDescription() const
{
	return TEXT("Set the BehaviorAsset property on a BTTask_RunBehavior node so it invokes a sub-behavior tree.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_SetSubtreeAsset::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("node_id"), TEXT("GUID of the RunBehavior task node."), true);
	Builder.AddString(TEXT("subtree_path"), TEXT("Asset path of the Behavior Tree to invoke."), true);
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_SetSubtreeAsset::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString SubtreePath;
	if (!Arguments->TryGetStringField(TEXT("subtree_path"), SubtreePath) || SubtreePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: subtree_path"));
	}

	UBehaviorTreeGraphNode* GraphNode = ClaireonBehaviorTreeHelpers::FindGraphNodeByGuid(Graph, NodeGuid);
	if (!GraphNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Node not found: %s"), *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower)));
	}

	UBTTask_RunBehavior* RunBehaviorNode = Cast<UBTTask_RunBehavior>(GraphNode->NodeInstance);
	if (!RunBehaviorNode)
	{
		return MakeErrorResult(TEXT("Node is not a BTTask_RunBehavior - set_subtree_asset only works on RunBehavior task nodes"));
	}

	UBehaviorTree* SubtreeBT = ClaireonBehaviorTreeHelpers::LoadBehaviorTreeAsset(SubtreePath, Error);
	if (!SubtreeBT)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Subtree Asset")));
	Data->BehaviorTree->Modify();

	if (!ClaireonBehaviorTreeHelpers::SetBTNodeProperty(RunBehaviorNode, TEXT("BehaviorAsset"), SubtreePath, Error))
	{
		// Fallback: try direct property assignment
		FProperty* BehaviorAssetProp = FindFProperty<FProperty>(RunBehaviorNode->GetClass(), TEXT("BehaviorAsset"));
		if (BehaviorAssetProp)
		{
			void* ValuePtr = BehaviorAssetProp->ContainerPtrToValuePtr<void>(RunBehaviorNode);
			FObjectProperty* ObjProp = CastField<FObjectProperty>(BehaviorAssetProp);
			if (ObjProp)
			{
				ObjProp->SetObjectPropertyValue(ValuePtr, SubtreeBT);
			}
		}
	}

	Data->FocusedNodeGuid = NodeGuid;
	Data->LastOperationStatus = FString::Printf(TEXT("set_subtree_asset - Set subtree to %s on {%s}"),
		*SubtreePath, *NodeGuid.ToString(EGuidFormats::DigitsWithHyphensLower));

	return BuildStateResponse(SessionId, Data);
}
