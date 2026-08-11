// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_ListGraphs.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
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


FString ClaireonBlueprintGraphTool_ListGraphs::GetOperation() const { return TEXT("list_graphs"); }

FString ClaireonBlueprintGraphTool_ListGraphs::GetDescription() const
{
    return TEXT("List all graphs in a Blueprint by asset_path. Stateless / read-only / non-session: never mutates and requires no open session. Returns event-graph, function-graph, and macro-graph names. Use the returned name with bp_open's graph_name parameter to start an editing session on a specific graph.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ListGraphs::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path."), true);
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_ListGraphs::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // list_graphs is stateless.
    TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
    if (Params->HasField(TEXT("params")))
    {
        const TSharedPtr<FJsonObject>* NestedObj = nullptr;
        if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
        {
            Params = *NestedObj;
        }
    }
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path. Stateless list_graphs requires: asset_path"));
	}

	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!IsValid(Blueprint))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));
	}

	// Collect all graphs with their type and node count
	struct FGraphEntry
	{
		FString Name;
		FString Type;
		int32 NodeCount;
	};
	TArray<FGraphEntry> Graphs;

	for (UEdGraph* Graph : Blueprint->UbergraphPages)
	{
		if (IsValid(Graph))
			Graphs.Add({ Graph->GetName(), TEXT("Ubergraph"), Graph->Nodes.Num() });
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!IsValid(Graph))
			continue;
		// UAnimationGraph instances in FunctionGraphs are anim graphs, not regular functions
		const FString GraphType = Cast<UAnimationGraph>(Graph) ? TEXT("AnimGraph") : TEXT("Function");
		Graphs.Add({ Graph->GetName(), GraphType, Graph->Nodes.Num() });
	}
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (IsValid(Graph))
			Graphs.Add({ Graph->GetName(), TEXT("Macro"), Graph->Nodes.Num() });
	}
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (IsValid(Graph))
			Graphs.Add({ Graph->GetName(), TEXT("DelegateSignature"), Graph->Nodes.Num() });
	}
	{
		// Collapsed-graph (Composite) bodies live in SubGraphs, not the top-level
		// arrays; list them too so replay can enumerate every reachable graph.
		TSet<FString> ListedNames;
		for (const FGraphEntry& Entry : Graphs)
		{
			ListedNames.Add(Entry.Name);
		}
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (UEdGraph* Graph : AllGraphs)
		{
			if (IsValid(Graph) && !ListedNames.Contains(Graph->GetName()))
			{
				Graphs.Add({ Graph->GetName(), TEXT("Subgraph"), Graph->Nodes.Num() });
				ListedNames.Add(Graph->GetName());
			}
		}
	}

	FString Output = FString::Printf(TEXT("Graphs in %s (%d total):\n"), *AssetPath, Graphs.Num());
	for (const FGraphEntry& Entry : Graphs)
	{
		Output += FString::Printf(TEXT("  %s  [%s, %d nodes]\n"),
			*Entry.Name, *Entry.Type, Entry.NodeCount);
	}

	// Structured payload: ClaireonBridge's BuildResultEnvelope substitutes {}
	// for a null Data object, so programmatic callers reading result["data"]
	// would otherwise always see an empty object (summary-only results).
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("asset_path"), AssetPath);
	ResultData->SetNumberField(TEXT("count"), Graphs.Num());
	TArray<TSharedPtr<FJsonValue>> GraphValues;
	GraphValues.Reserve(Graphs.Num());
	for (const FGraphEntry& Entry : Graphs)
	{
		TSharedPtr<FJsonObject> GraphJson = MakeShared<FJsonObject>();
		GraphJson->SetStringField(TEXT("name"), Entry.Name);
		GraphJson->SetStringField(TEXT("type"), Entry.Type);
		GraphJson->SetNumberField(TEXT("node_count"), Entry.NodeCount);
		GraphValues.Add(MakeShared<FJsonValueObject>(GraphJson));
	}
	ResultData->SetArrayField(TEXT("graphs"), GraphValues);

	return MakeSuccessResult(ResultData, Output);
}

#undef LOCTEXT_NAMESPACE
