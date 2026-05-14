// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_RemoveNode.h"
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


FString ClaireonBlueprintGraphTool_RemoveNode::GetOperation() const { return TEXT("graph_remove_node"); }

FString ClaireonBlueprintGraphTool_RemoveNode::GetDescription() const
{
    return TEXT("Remove a node from the graph. Requires open session_id from blueprint_graph_open OR pass asset_path for stateless single-shot mode. Transactional. All wires touching the node are dropped; if the node was the cursor target, the cursor is cleared and must be repositioned with blueprint_graph_select_node.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_RemoveNode::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create. Stateless when omitted (requires asset_path + graph_name)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (stateless mode, or alternative to session_id)."), false);
    Builder.AddString(TEXT("graph_name"), TEXT("Graph name (required in stateless mode)."), false);
    Builder.AddString(TEXT("node_guid"), TEXT("GUID of the node to remove."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_RemoveNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // remove_node: session-based when session_id present, stateless otherwise.
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
        if (!BeginSessionOp(Arguments, TEXT("remove_node"), SessionParams, SessionId, Data, Error))
        {
            return Error;
        }
        return CheckMutationAffectedNodes(TEXT("remove_node"), Data, RemoveNode_Impl(SessionId, Data, SessionParams));
    }

    return RemoveNodeStateless_Impl(Params);
}

FToolResult ClaireonBlueprintGraphTool_RemoveNode::RemoveNode_Impl(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	// Get node_guid
	FString NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));
	}

	// Find the node
	UEdGraphNode* Node = ClaireonBPGraphInternal::FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

	// Capture exec-connected neighbors BEFORE removing (they change after BreakAllPinLinks)
	for (UEdGraphPin* RemPin : Node->Pins)
	{
		if (RemPin && RemPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			for (UEdGraphPin* LinkedRemPin : RemPin->LinkedTo)
			{
				if (LinkedRemPin && LinkedRemPin->GetOwningNode())
				{
					Data->LastOperationAffectedNodes.Add(LinkedRemPin->GetOwningNode()->NodeGuid);
				}
			}
		}
	}

	// Remove the node using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Blueprint Node")));
	Blueprint->Modify();
	Graph->Modify();

	// Break all pin connections first
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			Pin->BreakAllPinLinks();
		}
	}

	// Remove from graph
	Graph->RemoveNode(Node);

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// Update cursor if it was pointing to removed node
	if (Data->Cursor.FocusedNodeGuid == NodeGuid)
	{
		ValidateCursor(Data);
	}

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Removed node: %s"), *NodeTitle);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonBlueprintGraphTool_RemoveNode::RemoveNodeStateless_Impl(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, GraphName, NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
		return MakeErrorResult(TEXT("Missing required field: asset_path. Stateless remove_node requires: asset_path, graph_name, node_guid"));
	if (!Params->TryGetStringField(TEXT("graph_name"), GraphName))
		return MakeErrorResult(TEXT("Missing required field: graph_name. Stateless remove_node requires: asset_path, graph_name, node_guid"));
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid. Stateless remove_node requires: asset_path, graph_name, node_guid"));

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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Blueprint Node")));
	Blueprint->Modify();
	Graph->Modify();

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
			Pin->BreakAllPinLinks();
	}
	Graph->RemoveNode(Node);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	return MakeSuccessResult(nullptr, FString::Printf(TEXT("Removed node '%s' (GUID: %s) from %s/%s"), *NodeTitle, *NodeGuidStr, *AssetPath, *GraphName));
}

#undef LOCTEXT_NAMESPACE
