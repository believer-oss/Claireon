// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_DisconnectPin.h"
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


FString ClaireonBlueprintGraphTool_DisconnectPin::GetOperation() const { return TEXT("graph_disconnect_pin"); }

FString ClaireonBlueprintGraphTool_DisconnectPin::GetDescription() const
{
    return TEXT("Disconnect a specific pin on a node (or all its connections) in the open Blueprint editing session. Requires open session_id from blueprint_graph_open (or pass asset_path to auto-open). Transactional. The pin remains on the node; only the wires are removed.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_DisconnectPin::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("node_title"), TEXT("Title of the target node."));
    Builder.AddString(TEXT("node_guid"), TEXT("GUID of the target node (alternative to node_title)."));
    Builder.AddString(TEXT("pin_name"), TEXT("Pin name to disconnect."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_DisconnectPin::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("disconnect_pin"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return CheckMutationAffectedNodes(TEXT("disconnect_pin"), Data, DisconnectPin_Impl(SessionId, Data, Params));
}

FToolResult ClaireonBlueprintGraphTool_DisconnectPin::DisconnectPin_Impl(
    const FString& SessionId,
    FBlueprintEditToolData* Data,
    const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	// Get pin (node is resolved via ResolveTargetNode below)
	FString PinName;
	if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
	{
		return MakeErrorResult(TEXT("Missing required field: pin_name"));
	}

	UEdGraphNode* Node = nullptr;
	FToolResult ResolveError;
	if (!ResolveTargetNode(Params, Graph, Node, ResolveError))
	{
		return ResolveError;
	}

	// Resolve pin using fuzzy matching
	TArray<FString> ResolutionWarnings;
	ClaireonNameResolver::FNameResolveResult DisconnectPinResult;
	UEdGraphPin* Pin = ClaireonNameResolver::ResolvePinName(Node, PinName, EGPD_MAX, DisconnectPinResult);
	if (!Pin)
	{
		return MakeErrorResult(DisconnectPinResult.Error);
	}
	if (!DisconnectPinResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(DisconnectPinResult.ResolutionNote);
	}

	int32 ConnectionCount = Pin->LinkedTo.Num();
	if (ConnectionCount == 0)
	{
		return MakeErrorResult(FString::Printf(TEXT("Pin '%s' has no connections to break"), *PinName));
	}

	// Capture both endpoint nodes BEFORE breaking (links gone after BreakAllPinLinks)
	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
	for (UEdGraphPin* LinkedDisconPin : Pin->LinkedTo)
	{
		if (LinkedDisconPin && LinkedDisconPin->GetOwningNode())
		{
			Data->LastOperationAffectedNodes.Add(LinkedDisconPin->GetOwningNode()->NodeGuid);
		}
	}

	// Break connections using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Disconnect Blueprint Pin")));
	Blueprint->Modify();
	Graph->Modify();

	Pin->BreakAllPinLinks();

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Disconnected %d connection(s) from [%s].%s"),
		ConnectionCount, *NodeTitle, *PinName);

	FToolResult DisconnectResult = BuildStateResponse(SessionId, Data);
	DisconnectResult.Warnings.Append(ResolutionWarnings);
	return DisconnectResult;
}

#undef LOCTEXT_NAMESPACE
