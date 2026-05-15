// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_ImportNodes.h"
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


FString ClaireonBlueprintGraphTool_ImportNodes::GetName() const
{
    return TEXT("claireon.blueprint_graph_import_nodes");
}

FString ClaireonBlueprintGraphTool_ImportNodes::GetDescription() const
{
    return TEXT("Import nodes from T3D text.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ImportNodes::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("t3d_text"), TEXT("T3D text blob to import."), true);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_ImportNodes::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("import_nodes"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_ImportNodes(SessionId, Data, Params);
}

FToolResult ClaireonBlueprintGraphEditToolBase::Operation_ImportNodes(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	// Get T3D text
	FString T3DText;
	if (!Params->TryGetStringField(TEXT("t3d_text"), T3DText))
	{
		return MakeErrorResult(TEXT("Missing required field: t3d_text"));
	}

	// Get optional offset position
	FVector2D Offset(0.0f, 0.0f);
	const TSharedPtr<FJsonObject>* OffsetObj = nullptr;
	if (Params->TryGetObjectField(TEXT("offset"), OffsetObj))
	{
		double X = 0.0, Y = 0.0;
		(*OffsetObj)->TryGetNumberField(TEXT("x"), X);
		(*OffsetObj)->TryGetNumberField(TEXT("y"), Y);
		Offset = FVector2D(X, Y);
	}

	// Import nodes using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Import Blueprint Nodes")));
	Blueprint->Modify();
	Graph->Modify();

	// Use FEdGraphUtilities to import nodes from text
	TSet<UEdGraphNode*> ImportedNodes;
	FEdGraphUtilities::ImportNodesFromText(Graph, T3DText, ImportedNodes);

	if (ImportedNodes.Num() == 0)
	{
		return MakeErrorResult(TEXT("Failed to import nodes from T3D text (invalid format or empty)"));
	}

	// Apply offset if specified
	if (!Offset.IsZero())
	{
		for (UEdGraphNode* Node : ImportedNodes)
		{
			if (Node)
			{
				Node->NodePosX += Offset.X;
				Node->NodePosY += Offset.Y;
			}
		}
	}

	// Mark Blueprint as structurally modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// Move cursor to first imported node
	if (ImportedNodes.Num() > 0)
	{
		UEdGraphNode* FirstNode = nullptr;
		for (UEdGraphNode* Node : ImportedNodes)
		{
			FirstNode = Node;
			break;
		}

		if (FirstNode)
		{
			Data->Cursor.PushHistory(Data->Cursor.GraphName);
			Data->Cursor.FocusedNodeGuid = FirstNode->NodeGuid;

			UEdGraphPin* FirstOutputPin = ClaireonBlueprintHelpers::GetFirstOutputPin(FirstNode);
			if (FirstOutputPin)
			{
				Data->Cursor.FocusedPinName = FirstOutputPin->PinName;
				Data->Cursor.FocusedPinDirection = FirstOutputPin->Direction;
			}
			else
			{
				Data->Cursor.FocusedPinName = NAME_None;
			}
		}
	}

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Imported %d node(s) from T3D text"),
		ImportedNodes.Num());

	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
