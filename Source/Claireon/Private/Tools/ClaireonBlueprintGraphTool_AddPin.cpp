// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_AddPin.h"
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
#include "K2Node_EditablePinBase.h"
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


FString ClaireonBlueprintGraphTool_AddPin::GetOperation() const { return TEXT("add_pin"); }

FString ClaireonBlueprintGraphTool_AddPin::GetDescription() const
{
    return TEXT("Add a dynamic pin to a node in the open Blueprint editing session. Requires open session_id from bp_open (or pass asset_path to auto-open). Transactional. Only nodes that implement AddPin support this (Sequence, MakeArray, Switch*, DoOnceMultiInput, etc.); non-supporting nodes error. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddPin::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("node_title"), TEXT("Title of the target node."));
    Builder.AddString(TEXT("node_guid"), TEXT("GUID of the target node (alternative to node_title)."));
    Builder.AddString(TEXT("pin_case"), TEXT("Optional case value for SwitchInteger/SwitchString/SwitchName."));
    Builder.AddString(TEXT("pin_name"), TEXT("With pin_type: create a USER-DEFINED pin of this name on a Tunnel/FunctionEntry/FunctionResult/CustomEvent node (this is how collapsed-graph tunnel pins are authored)."));
    Builder.AddString(TEXT("pin_type"), TEXT("Type for the user-defined pin: 'exec' or any variable_type string (e.g. 'Boolean', 'Actor', 'Transform')."));
    Builder.AddString(TEXT("pin_direction"), TEXT("'input' | 'output' for the user-defined pin. Defaults to whichever direction the node supports (entry tunnels emit outputs, result tunnels take inputs)."));
    // Read at Execute and never declared until now.
    Builder.AddString(TEXT("pin_value"), TEXT("Optional default value for the new pin, in the pin type's ImportText form."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_AddPin::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("add_pin"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return CheckMutationAffectedNodes(TEXT("add_pin"), Data, AddPin_Impl(SessionId, Data, Params));
}

FToolResult ClaireonBlueprintGraphTool_AddPin::AddPin_Impl(
    const FString& SessionId,
    FBlueprintEditToolData* Data,
    const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();
	if (!IsValid(Blueprint) || !IsValid(Graph))
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));

	UEdGraphNode* Node = nullptr;
	FToolResult ResolveError;
	if (!ResolveTargetNode(Params, Graph, Node, ResolveError))
	{
		return ResolveError;
	}

	int32 Count = 1;
	Params->TryGetNumberField(TEXT("count"), Count);
	Count = FMath::Clamp(Count, 1, 50);

	FString PinValue;
	Params->TryGetStringField(TEXT("pin_value"), PinValue);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Blueprint Pin")));
	Blueprint->Modify();

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	int32 PinsAdded = 0;

	// User-defined pin path: pin_name + pin_type on a UK2Node_EditablePinBase
	// (Tunnel, FunctionEntry, FunctionResult, CustomEvent). This is how collapsed
	// graphs' tunnel pins are authored -- composite instance pins derive from them.
	FString UserPinName, UserPinType;
	if (Params->TryGetStringField(TEXT("pin_name"), UserPinName)
		&& Params->TryGetStringField(TEXT("pin_type"), UserPinType)
		&& !UserPinName.IsEmpty())
	{
		UK2Node_EditablePinBase* EditableNode = Cast<UK2Node_EditablePinBase>(Node);
		if (!IsValid(EditableNode))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Node '%s' (%s) does not support user-defined pins (expected a Tunnel/FunctionEntry/FunctionResult/CustomEvent-style node)"),
				*NodeTitle, *Node->GetClass()->GetName()));
		}

		FEdGraphPinType NewPinType;
		if (UserPinType.Equals(TEXT("exec"), ESearchCase::IgnoreCase))
		{
			NewPinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
		}
		else
		{
			ClaireonBlueprintHelpers::FParseVariableTypeResult ParseResult =
				ClaireonBlueprintHelpers::ParseVariableTypeChecked(UserPinType);
			if (!ParseResult.bSucceeded)
			{
				return MakeErrorResult(FString::Printf(
					TEXT("add_pin: failed to parse pin_type '%s': %s"), *UserPinType, *ParseResult.Error));
			}
			NewPinType = ParseResult.PinType;
		}

		// Direction: explicit pin_direction wins; otherwise derive from what the
		// node can carry (entry tunnels emit outputs, result tunnels take inputs).
		EEdGraphPinDirection Direction = EGPD_Output;
		FString DirectionStr;
		if (Params->TryGetStringField(TEXT("pin_direction"), DirectionStr))
		{
			Direction = DirectionStr.Equals(TEXT("input"), ESearchCase::IgnoreCase) ? EGPD_Input : EGPD_Output;
		}
		else
		{
			FText DirProbeError;
			if (!EditableNode->CanCreateUserDefinedPin(NewPinType, EGPD_Output, DirProbeError))
			{
				Direction = EGPD_Input;
			}
		}

		UEdGraphPin* NewPin = EditableNode->CreateUserDefinedPin(FName(*UserPinName), NewPinType, Direction);
		if (!NewPin)
		{
			// Direction may be unsupported on this node; try the opposite before failing.
			Direction = (Direction == EGPD_Output) ? EGPD_Input : EGPD_Output;
			NewPin = EditableNode->CreateUserDefinedPin(FName(*UserPinName), NewPinType, Direction);
		}
		if (!NewPin)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("CreateUserDefinedPin failed for '%s' (%s) on node '%s'"),
				*UserPinName, *UserPinType, *NodeTitle));
		}

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
		Data->Cursor.LastOperationStatus = FString::Printf(
			TEXT("Added user-defined pin '%s' (%s, %s) to node: %s"),
			*UserPinName, *UserPinType, Direction == EGPD_Input ? TEXT("input") : TEXT("output"), *NodeTitle);
		Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
		return BuildStateResponse(SessionId, Data);
	}

	// Try IK2Node_AddPinInterface first
	IK2Node_AddPinInterface* AddPinIface = Cast<IK2Node_AddPinInterface>(Node);
	if (AddPinIface)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			if (!AddPinIface->CanAddPin())
			{
				break;
			}
			AddPinIface->AddInputPin();
			++PinsAdded;
		}
	}
	// Try Switch nodes
	else if (UK2Node_Switch* SwitchNode = Cast<UK2Node_Switch>(Node); IsValid(SwitchNode))
	{
		if (SwitchNode->IsA<UK2Node_SwitchEnum>())
		{
			return MakeErrorResult(TEXT("SwitchEnum pins are fixed to enum entries and cannot be added dynamically"));
		}

		for (int32 i = 0; i < Count; ++i)
		{
			if (!PinValue.IsEmpty())
			{
				// For String/Name switches, directly add to PinNames for a specific case value
				if (UK2Node_SwitchString* StringSwitch = Cast<UK2Node_SwitchString>(SwitchNode); IsValid(StringSwitch))
				{
					StringSwitch->PinNames.Add(FName(*PinValue));
					// Append index suffix for subsequent pins in batch
					if (i > 0)
					{
						StringSwitch->PinNames.Last() = FName(*FString::Printf(TEXT("%s_%d"), *PinValue, i));
					}
				}
				else if (UK2Node_SwitchName* NameSwitch = Cast<UK2Node_SwitchName>(SwitchNode); IsValid(NameSwitch))
				{
					NameSwitch->PinNames.Add(FName(*PinValue));
					if (i > 0)
					{
						NameSwitch->PinNames.Last() = FName(*FString::Printf(TEXT("%s_%d"), *PinValue, i));
					}
				}
				else
				{
					// Integer switch -- ignore pin_value, use auto-numbering
					SwitchNode->AddPinToSwitchNode();
				}
			}
			else
			{
				SwitchNode->AddPinToSwitchNode();
			}
			++PinsAdded;
		}

		// If we added by manipulating PinNames, reconstruct to create the actual pins
		if (!PinValue.IsEmpty())
		{
			Node->ReconstructNode();
		}
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Node '%s' does not support adding pins. Supported: nodes implementing IK2Node_AddPinInterface (Sequence, MakeArray, MakeSet, MakeMap, Select, DoOnceMultiInput) and Switch nodes (SwitchInteger, SwitchString, SwitchName)."), *NodeTitle));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Added %d pin(s) to node: %s"), PinsAdded, *NodeTitle);
	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
