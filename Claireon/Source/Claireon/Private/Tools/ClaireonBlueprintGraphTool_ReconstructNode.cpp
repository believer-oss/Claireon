// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_ReconstructNode.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "Dom/JsonObject.h"
#include "Tools/ClaireonSpecApplicator_Blueprint.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_Event.h"
#include "K2Node_CallParentFunction.h"
#include "K2Node_Timeline.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_Select.h"
#include "K2Node_MacroInstance.h"
#include "Engine/MemberReference.h"
#include "K2Node_DynamicCast.h"
#include "K2Node_MakeStruct.h"
#include "K2Node_BreakStruct.h"
#include "K2Node_SpawnActorFromClass.h"
#include "K2Node_Knot.h"
#include "EdGraphNode_Comment.h"
#include "K2Node_Literal.h"
#include "K2Node_MakeArray.h"
#include "K2Node_MakeMap.h"
#include "K2Node_MakeSet.h"
#include "K2Node_GetArrayItem.h"
#include "K2Node_AddPinInterface.h"
#include "K2Node_Switch.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_ForEachElementInEnum.h"
#include "K2Node_DoOnceMultiInput.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_ComponentBoundEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphUtilities.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "ScopedTransaction.h"
#include "Animation/AnimBlueprint.h"
#include "AnimationGraph.h"
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_Root.h"
#include "K2Node_Tunnel.h"
#include "ClaireonBlueprintNodeSerializer.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonBPInterfaceAuthor.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

#define LOCTEXT_NAMESPACE "ClaireonBlueprintGraphEditToolBase"

using FToolResult = IClaireonTool::FToolResult;


FString ClaireonBlueprintGraphTool_ReconstructNode::GetOperation() const { return TEXT("graph_reconstruct_node"); }

FString ClaireonBlueprintGraphTool_ReconstructNode::GetDescription() const
{
    return TEXT("Reconstruct a node in place to refresh its pins from the current class definition. Requires open session_id from blueprint_graph_open OR pass asset_path for stateless single-shot mode. Transactional. Use after editing the underlying UFUNCTION/struct so the node picks up the new pin layout.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ReconstructNode::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create. Stateless when omitted (requires asset_path + graph_name)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (stateless mode, or alternative to session_id)."), false);
    Builder.AddString(TEXT("graph_name"), TEXT("Graph name (required in stateless mode)."), false);
    Builder.AddString(TEXT("node_guid"), TEXT("GUID of the node to reconstruct."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_ReconstructNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // reconstruct_node: session-based when session_id present, stateless otherwise.
    TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* NestedObj = nullptr;
        if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
        {
            Params = *NestedObj;
        }
    }

    FString SessionId;
    if (Params->TryGetStringField(TEXT("session_id"), SessionId) && !SessionId.IsEmpty())
    {
        TSharedPtr<FJsonObject> SessionParams;
        FBlueprintEditToolData* Data = nullptr;
        FToolResult Error;
        if (!BeginSessionOp(Arguments, TEXT("reconstruct_node"), SessionParams, SessionId, Data, Error))
        {
            return Error;
        }
        return ReconstructNode_Impl(SessionId, Data, SessionParams);
    }

    return ReconstructNodeStateless_Impl(Params);
}

FToolResult ClaireonBlueprintGraphTool_ReconstructNode::ReconstructNode_Impl(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();
	if (!Blueprint || !Graph)
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));

	FString NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));

	UEdGraphNode* Node = ClaireonBPGraphInternal::FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Reconstruct Blueprint Node")));
	Blueprint->Modify();
	Node->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Reconstructed node: %s"), *NodeTitle);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonBlueprintGraphTool_ReconstructNode::ReconstructNodeStateless_Impl(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, GraphName, NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
		return MakeErrorResult(TEXT("Missing required field: asset_path. Stateless reconstruct_node requires: asset_path, graph_name, node_guid"));
	if (!Params->TryGetStringField(TEXT("graph_name"), GraphName))
		return MakeErrorResult(TEXT("Missing required field: graph_name. Stateless reconstruct_node requires: asset_path, graph_name, node_guid"));
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid. Stateless reconstruct_node requires: asset_path, graph_name, node_guid"));

	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
		return MakeErrorResult(ValidationError);

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));

	UEdGraph* Graph = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, GraphName);
	if (!Graph)
		return MakeErrorResult(FString::Printf(TEXT("Graph '%s' not found"), *GraphName));

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));

	UEdGraphNode* Node = ClaireonBPGraphInternal::FindNodeForOperation(Graph, NodeGuid, nullptr);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Reconstruct Blueprint Node")));
	Blueprint->Modify();
	Node->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	return MakeSuccessResult(nullptr, FString::Printf(TEXT("Reconstructed node '%s' (GUID: %s) in %s/%s. Compile to apply."), *NodeTitle, *NodeGuidStr, *AssetPath, *GraphName));
}

#undef LOCTEXT_NAMESPACE
