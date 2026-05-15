// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_ConnectPins.h"
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


FString ClaireonBlueprintGraphTool_ConnectPins::GetName() const
{
    return TEXT("claireon.blueprint_graph_connect_pins");
}

FString ClaireonBlueprintGraphTool_ConnectPins::GetDescription() const
{
    return TEXT("Connect two pins. Accepts node GUIDs or titles plus pin names.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ConnectPins::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("source_node_title"), TEXT("Title of the source node."));
    Builder.AddString(TEXT("source_node_guid"), TEXT("GUID of the source node (alternative to source_node_title)."));
    Builder.AddString(TEXT("source_pin"), TEXT("Source pin name."), true);
    Builder.AddString(TEXT("target_node_title"), TEXT("Title of the target node."));
    Builder.AddString(TEXT("target_node_guid"), TEXT("GUID of the target node (alternative to target_node_title)."));
    Builder.AddString(TEXT("target_pin"), TEXT("Target pin name."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_ConnectPins::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("connect_pins"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return CheckMutationAffectedNodes(TEXT("connect_pins"), Data, Operation_ConnectPins(SessionId, Data, Params));
}

FToolResult ClaireonBlueprintGraphEditToolBase::Operation_ConnectPins(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	// Get source pin name (required)
	FString SourcePinName;
	if (!Params->TryGetStringField(TEXT("source_pin_name"), SourcePinName))
	{
		return MakeErrorResult(TEXT("Missing required field: source_pin_name"));
	}

	// Get target pin name (required)
	FString TargetPinName;
	if (!Params->TryGetStringField(TEXT("target_pin_name"), TargetPinName))
	{
		return MakeErrorResult(TEXT("Missing required field: target_pin_name"));
	}

	// Find source node (by GUID or title)
	UEdGraphNode* SourceNode = nullptr;
	FString SourceNodeGuidStr, SourceNodeTitle;

	if (Params->TryGetStringField(TEXT("source_node_guid"), SourceNodeGuidStr))
	{
		// Find by GUID
		FGuid SourceNodeGuid;
		if (!FGuid::Parse(SourceNodeGuidStr, SourceNodeGuid))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid source_node_guid format: %s"), *SourceNodeGuidStr));
		}
		SourceNode = ClaireonBPGraphInternal::FindNodeForOperation(Graph, SourceNodeGuid, Data);
		if (!SourceNode)
		{
			FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
			return MakeErrorResult(FString::Printf(TEXT("Source node not found with GUID: %s in graph '%s'.\n%s"),
				*SourceNodeGuidStr, *Graph->GetName(), *AvailableNodes));
		}
	}
	else if (Params->TryGetStringField(TEXT("source_node_title"), SourceNodeTitle))
	{
		// Find by title
		TArray<UEdGraphNode*> MatchingNodes = ClaireonBlueprintHelpers::FindNodesByTitle(Graph, SourceNodeTitle, true);
		if (MatchingNodes.Num() == 0)
		{
			return MakeErrorResult(FString::Printf(TEXT("Source node not found with title: %s"), *SourceNodeTitle));
		}
		else if (MatchingNodes.Num() > 1)
		{
			return MakeErrorResult(FString::Printf(TEXT("Ambiguous source node title '%s' - %d nodes match. Use source_node_guid instead."), *SourceNodeTitle, MatchingNodes.Num()));
		}
		SourceNode = MatchingNodes[0];
	}
	else
	{
		return MakeErrorResult(TEXT("Missing required field: source_node_guid or source_node_title"));
	}

	// Find target node (by GUID or title)
	UEdGraphNode* TargetNode = nullptr;
	FString TargetNodeGuidStr, TargetNodeTitle;

	if (Params->TryGetStringField(TEXT("target_node_guid"), TargetNodeGuidStr))
	{
		// Find by GUID
		FGuid TargetNodeGuid;
		if (!FGuid::Parse(TargetNodeGuidStr, TargetNodeGuid))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid target_node_guid format: %s"), *TargetNodeGuidStr));
		}
		TargetNode = ClaireonBPGraphInternal::FindNodeForOperation(Graph, TargetNodeGuid, Data);
		if (!TargetNode)
		{
			FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
			return MakeErrorResult(FString::Printf(TEXT("Target node not found with GUID: %s in graph '%s'.\n%s"),
				*TargetNodeGuidStr, *Graph->GetName(), *AvailableNodes));
		}
	}
	else if (Params->TryGetStringField(TEXT("target_node_title"), TargetNodeTitle))
	{
		// Find by title
		TArray<UEdGraphNode*> MatchingNodes = ClaireonBlueprintHelpers::FindNodesByTitle(Graph, TargetNodeTitle, true);
		if (MatchingNodes.Num() == 0)
		{
			return MakeErrorResult(FString::Printf(TEXT("Target node not found with title: %s"), *TargetNodeTitle));
		}
		else if (MatchingNodes.Num() > 1)
		{
			return MakeErrorResult(FString::Printf(TEXT("Ambiguous target node title '%s' - %d nodes match. Use target_node_guid instead."), *TargetNodeTitle, MatchingNodes.Num()));
		}
		TargetNode = MatchingNodes[0];
	}
	else
	{
		return MakeErrorResult(TEXT("Missing required field: target_node_guid or target_node_title"));
	}

	// Resolve source pin using fuzzy matching
	TArray<FString> ResolutionWarnings;
	EEdGraphPinDirection SourceDirHint = EGPD_Output;
	FString SourcePinDirection;
	if (Params->TryGetStringField(TEXT("source_pin_direction"), SourcePinDirection))
	{
		SourceDirHint = (SourcePinDirection == TEXT("input")) ? EGPD_Input : EGPD_Output;
	}
	ClaireonNameResolver::FNameResolveResult SourcePinResult;
	UEdGraphPin* SourcePin = ClaireonNameResolver::ResolvePinName(SourceNode, SourcePinName, SourceDirHint, SourcePinResult);
	if (!SourcePin)
	{
		return MakeErrorResult(SourcePinResult.Error);
	}
	if (!SourcePinResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(SourcePinResult.ResolutionNote);
	}

	// Resolve target pin using fuzzy matching
	EEdGraphPinDirection TargetDirHint = EGPD_Input;
	FString TargetPinDirection;
	if (Params->TryGetStringField(TEXT("target_pin_direction"), TargetPinDirection))
	{
		TargetDirHint = (TargetPinDirection == TEXT("input")) ? EGPD_Input : EGPD_Output;
	}
	ClaireonNameResolver::FNameResolveResult TargetPinResult;
	UEdGraphPin* TargetPin = ClaireonNameResolver::ResolvePinName(TargetNode, TargetPinName, TargetDirHint, TargetPinResult);
	if (!TargetPin)
	{
		return MakeErrorResult(TargetPinResult.Error);
	}
	if (!TargetPinResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(TargetPinResult.ResolutionNote);
	}

	// Use the schema's canonical TryCreateConnection path rather than the
	// raw MakeLinkTo + manual notification. TryCreateConnection:
	//   1. Calls CanCreateConnection and honours BREAK_OTHERS_A/B/AB responses.
	//   2. Calls both MakeLinkTo endpoints.
	//   3. Calls PinConnectionListChanged on both endpoints (the per-pin hook).
	//   4. On K2 graphs, NotifyPinConnectionListChanged is invoked via the
	//      K2 schema override to propagate wildcard types through
	//      UK2Node_CallArrayFunction, UK2Node_Select, UK2Node_MakeArray, etc.
	// This is the path the Blueprint editor itself uses, so going through it
	// keeps Claireon's behaviour identical to the UI's.
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

	// Pre-check so we can surface a clean error without a transaction and
	// without mutating the graph.
	const FPinConnectionResponse PreCheck = K2Schema->CanCreateConnection(SourcePin, TargetPin);
	if (PreCheck.Response == CONNECT_RESPONSE_DISALLOW)
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot connect pins: %s"), *PreCheck.Message.ToString()));
	}

	// Make the connection using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Connect Blueprint Pins")));
	Blueprint->Modify();
	Graph->Modify();

	const bool bConnectionMade = K2Schema->TryCreateConnection(SourcePin, TargetPin);
	if (!bConnectionMade)
	{
		return MakeErrorResult(TEXT("TryCreateConnection failed (schema rejected the link)"));
	}

	// Capture node titles AFTER TryCreateConnection has run (wildcard
	// propagation can rebuild pin arrays, but the node objects survive).
	const FString SourceTitle = SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
	const FString TargetTitle = TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString();

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Connected: [%s].%s -> [%s].%s"),
		*SourceTitle, *SourcePinName,
		*TargetTitle, *TargetPinName);

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Populate affected nodes: both endpoint nodes
	Data->LastOperationAffectedNodes.Add(SourceNode->NodeGuid);
	Data->LastOperationAffectedNodes.Add(TargetNode->NodeGuid);

	// Wildcard propagation may re-type sibling pins and trigger ReconstructNode
	// on the endpoint nodes, which can re-link to (or unlink from) neighbors.
	// Add any currently-linked neighbor GUIDs to the affected set so the diff
	// response reflects the full extent of the change.
	auto AddLinkedNeighborGuids = [Data](UEdGraphNode* EndpointNode)
	{
		if (!EndpointNode) { return; }
		for (UEdGraphPin* Pin : EndpointNode->Pins)
		{
			if (!Pin) { continue; }
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked)
				{
					if (UEdGraphNode* Neighbor = Linked->GetOwningNodeUnchecked())
					{
						Data->LastOperationAffectedNodes.Add(Neighbor->NodeGuid);
					}
				}
			}
		}
	};
	AddLinkedNeighborGuids(SourceNode);
	AddLinkedNeighborGuids(TargetNode);

	FToolResult ConnectResult = BuildStateResponse(SessionId, Data);
	ConnectResult.Warnings.Append(ResolutionWarnings);
	return ConnectResult;
}

#undef LOCTEXT_NAMESPACE
