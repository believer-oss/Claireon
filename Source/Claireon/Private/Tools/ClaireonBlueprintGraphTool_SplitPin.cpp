// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_SplitPin.h"
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


FString ClaireonBlueprintGraphTool_SplitPin::GetOperation() const { return TEXT("split_pin"); }

FString ClaireonBlueprintGraphTool_SplitPin::GetDescription() const
{
    return TEXT("Split a struct pin into its component sub-pins in the open Blueprint editing session. Requires open session_id from bp_open (or pass asset_path to auto-open). Transactional. Common pitfalls: only struct-typed pins are splittable; a pin with existing connections refuses to split (disconnect first, or split immediately after node creation before wiring).");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_SplitPin::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("node_guid"), TEXT("GUID of the node that owns the pin."), true);
    Builder.AddString(TEXT("pin_name"), TEXT("Name of the struct pin to split."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_SplitPin::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("split_pin"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return CheckMutationAffectedNodes(TEXT("split_pin"), Data, SplitPin_Impl(SessionId, Data, Params));
}

FToolResult ClaireonBlueprintGraphTool_SplitPin::SplitPin_Impl(
    const FString& SessionId,
    FBlueprintEditToolData* Data,
    const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();
	if (!IsValid(Blueprint) || !IsValid(Graph))
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));

	FString NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));

	FString PinName;
	if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
		return MakeErrorResult(TEXT("Missing required field: pin_name"));

	FString ResolveError;
	UEdGraphNode* Node = ClaireonBPGraphInternal::FindNodeForOperationStr(Graph, NodeGuidStr, Data, ResolveError);
	if (!IsValid(Node))
	{
		return MakeErrorResult(ResolveError);
	}

	TArray<FString> ResolutionWarnings;
	ClaireonNameResolver::FNameResolveResult SplitPinResult;
	UEdGraphPin* Pin = ClaireonNameResolver::ResolvePinName(Node, PinName, EGPD_MAX, SplitPinResult);
	if (!Pin)
	{
		return MakeErrorResult(SplitPinResult.Error);
	}
	if (!SplitPinResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(SplitPinResult.ResolutionNote);
	}

	if (Pin->SubPins.Num() > 0)
	{
		return MakeErrorResult(FString::Printf(TEXT("Pin '%s' is already split"), *PinName));
	}

	// Splitting a connected pin hides the parent pin but leaves its LinkedTo intact on
	// the far side; downstream churn then trashes pins that are still referenced, which
	// asserts during save and corrupts the asset. Refuse loudly instead (the editor UI
	// disables Split for connected pins for the same reason).
	if (Pin->LinkedTo.Num() > 0)
	{
		TArray<FString> LinkDescs;
		for (const UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (Linked && IsValid(Linked->GetOwningNodeUnchecked()))
			{
				LinkDescs.Add(FString::Printf(TEXT("'%s' on '%s'"), *Linked->PinName.ToString(),
					*Linked->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString()));
			}
		}
		return MakeErrorResult(FString::Printf(
			TEXT("Pin '%s' has %d existing connection(s) (%s) and cannot be split safely. ")
			TEXT("Split pins immediately after node creation (before connecting), or bp_disconnect_pin first and reconnect to the sub-pins after the split."),
			*PinName, Pin->LinkedTo.Num(), *FString::Join(LinkDescs, TEXT(", "))));
	}

	if (!Node->CanSplitPin(Pin))
	{
		return MakeErrorResult(FString::Printf(TEXT("Pin '%s' cannot be split (not a struct pin, or the node forbids splitting it)"), *PinName));
	}

	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Split Blueprint Pin")));
	Blueprint->Modify();

	K2Schema->SplitPin(Pin);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// Collect sub-pin names for reporting
	TArray<FString> SubPinNames;
	for (UEdGraphPin* SubPin : Pin->SubPins)
	{
		if (SubPin)
			SubPinNames.Add(SubPin->PinName.ToString());
	}

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Split pin '%s' on node '%s' into: %s"),
		*PinName, *NodeTitle, *FString::Join(SubPinNames, TEXT(", ")));
	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
	FToolResult SplitPinFinalResult = BuildStateResponse(SessionId, Data);
	SplitPinFinalResult.Warnings.Append(ResolutionWarnings);
	return SplitPinFinalResult;
}

#undef LOCTEXT_NAMESPACE
