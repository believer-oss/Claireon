// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_MoveNode.h"
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


FString ClaireonBlueprintGraphTool_MoveNode::GetOperation() const { return TEXT("move_node"); }

FString ClaireonBlueprintGraphTool_MoveNode::GetDescription() const
{
    return TEXT("Move a node to a new position in the graph in the open Blueprint editing session. Requires open session_id from bp_open (or pass asset_path to auto-open). Transactional. Layout-only: connections, properties, and pin values are unchanged. Use bp_format for whole-graph cleanup. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_MoveNode::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("node_title"), TEXT("Title of the target node."));
    Builder.AddString(TEXT("node_guid"), TEXT("GUID of the target node (alternative to node_title)."));
    Builder.AddNumber(TEXT("position_x"), TEXT("New X coordinate. Required together with position_y unless a position={x,y} object is supplied; the object wins when both forms are present."));
    Builder.AddNumber(TEXT("position_y"), TEXT("New Y coordinate. Required together with position_x unless a position={x,y} object is supplied."));
    Builder.AddObject(TEXT("position"), TEXT("{x,y} position object; alternative to position_x/position_y. Must contain BOTH x and y when present -- a partial object is an error, never a zero-default. Wins over the scalar form when both are supplied."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_MoveNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("move_node"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return CheckMutationAffectedNodes(TEXT("move_node"), Data, MoveNode_Impl(SessionId, Data, Params));
}

FToolResult ClaireonBlueprintGraphTool_MoveNode::MoveNode_Impl(
    const FString& SessionId,
    FBlueprintEditToolData* Data,
    const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!IsValid(Blueprint) || !IsValid(Graph))
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	// Get position. Accept the schema's position_x/position_y number fields and
	// a position={x,y} object -- earlier builds validated only the object form
	// while the schema advertised the kwargs, forcing callers to pass both.
	// The object form wins when both forms are supplied. A partial object is an
	// error, never a silent move to origin.
	double X = 0.0, Y = 0.0;
	const TSharedPtr<FJsonObject>* PositionObj = nullptr;
	if (Params->TryGetObjectField(TEXT("position"), PositionObj))
	{
		double ObjX = 0.0, ObjY = 0.0;
		const bool bObjHasX = (*PositionObj)->TryGetNumberField(TEXT("x"), ObjX);
		const bool bObjHasY = (*PositionObj)->TryGetNumberField(TEXT("y"), ObjY);
		if (!bObjHasX || !bObjHasY)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("position object is missing '%s'; pass both x and y (a partial position object is never zero-defaulted)"),
				!bObjHasX ? TEXT("x") : TEXT("y")));
		}
		X = ObjX;
		Y = ObjY;
	}
	else
	{
		const bool bHaveX = Params->TryGetNumberField(TEXT("position_x"), X);
		const bool bHaveY = Params->TryGetNumberField(TEXT("position_y"), Y);
		if (!bHaveX || !bHaveY)
		{
			return MakeErrorResult(TEXT("Missing required position: pass position_x and position_y (or a position={x,y} object)"));
		}
	}

	UEdGraphNode* Node = nullptr;
	FToolResult ResolveError;
	if (!ResolveTargetNode(Params, Graph, Node, ResolveError))
	{
		return ResolveError;
	}

	// Move the node
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Move Blueprint Node")));
	Node->Modify();
	// NodePosX/Y are int32; convert explicitly (JSON numbers arrive as double).
	Node->NodePosX = FMath::RoundToInt32(X);
	Node->NodePosY = FMath::RoundToInt32(Y);
	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Moved node %s to (%.0f, %.0f)"),
		*Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens).Left(8), X, Y);

	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
