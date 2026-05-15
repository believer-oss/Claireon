// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_Format.h"
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


FString ClaireonBlueprintGraphTool_Format::GetName() const
{
    return TEXT("claireon.blueprint_graph_format");
}

FString ClaireonBlueprintGraphTool_Format::GetDescription() const
{
    return TEXT("Auto-layout the current graph's nodes.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_Format::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_Format::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("format"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_Format(SessionId, Data, Params);
}

FToolResult ClaireonBlueprintGraphEditToolBase::Operation_Format(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	// Use the fallback formatter (simple topological layout)
	// Find root nodes (event nodes)
	TArray<UEdGraphNode*> RootNodes = ClaireonBlueprintHelpers::FindRootNodes(Graph);

	if (RootNodes.Num() == 0)
	{
		Data->Cursor.LastOperationStatus = TEXT("No root nodes found to format");
		return BuildStateResponse(SessionId, Data);
	}

	// Simple layout: place root nodes vertically, then place connected nodes to the right
	const float VerticalSpacing = 150.0f;
	const float HorizontalSpacing = 300.0f;
	float CurrentY = 0.0f;

	for (UEdGraphNode* RootNode : RootNodes)
	{
		// Place root node
		RootNode->NodePosX = 0.0f;
		RootNode->NodePosY = CurrentY;

		// Place connected nodes
		TArray<UEdGraphNode*> VisitedNodes;
		VisitedNodes.Add(RootNode);

		float ColumnX = HorizontalSpacing;
		TArray<UEdGraphNode*> CurrentColumn;
		CurrentColumn.Add(RootNode);

		while (CurrentColumn.Num() > 0)
		{
			TArray<UEdGraphNode*> NextColumn;
			float ColumnY = CurrentY;

			for (UEdGraphNode* Node : CurrentColumn)
			{
				// Find connected nodes via exec pins
				TArray<UEdGraphPin*> ExecOutputs = ClaireonBlueprintHelpers::GetExecPins(Node, false, true);
				for (UEdGraphPin* ExecPin : ExecOutputs)
				{
					for (UEdGraphPin* LinkedPin : ExecPin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode())
						{
							UEdGraphNode* LinkedNode = LinkedPin->GetOwningNode();
							if (!VisitedNodes.Contains(LinkedNode))
							{
								VisitedNodes.Add(LinkedNode);
								LinkedNode->NodePosX = ColumnX;
								LinkedNode->NodePosY = ColumnY;
								ColumnY += VerticalSpacing;
								NextColumn.Add(LinkedNode);
							}
						}
					}
				}
			}

			CurrentColumn = NextColumn;
			ColumnX += HorizontalSpacing;
		}

		CurrentY += VerticalSpacing * 2.0f; // Extra space between root node chains
	}

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Formatted graph (simple layout, %d nodes positioned)"), RootNodes.Num());
	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
