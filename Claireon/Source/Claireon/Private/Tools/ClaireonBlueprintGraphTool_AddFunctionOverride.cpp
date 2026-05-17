// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_AddFunctionOverride.h"
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


FString ClaireonBlueprintGraphTool_AddFunctionOverride::GetOperation() const { return TEXT("add_function_override"); }

FString ClaireonBlueprintGraphTool_AddFunctionOverride::GetDescription() const
{
    return TEXT("Create a function-override graph for a BlueprintNativeEvent or BlueprintImplementableEvent in the open editing session. Requires open session_id from blueprint_graph_open (or pass asset_path to auto-open). Transactional. The override target must be declared on the parent class or a UFUNCTION-marked interface. Session-mode tool: open via blueprint_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddFunctionOverride::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("function_name"), TEXT("Name of the parent function to override."), true);
    Builder.AddString(TEXT("interface_class"), TEXT("Optional interface class path if overriding an interface function."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_AddFunctionOverride::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("add_function_override"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return CheckMutationAffectedNodes(TEXT("add_function_override"), Data, AddFunctionOverride_Impl(SessionId, Data, Params));
}

FToolResult ClaireonBlueprintGraphTool_AddFunctionOverride::AddFunctionOverride_Impl(
    const FString& SessionId,
    FBlueprintEditToolData* Data,
    const TSharedPtr<FJsonObject>& Params)
{
	// 1. Extract function_name (required)
	FString FunctionName;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
	{
		return MakeErrorResult(TEXT("Missing required field 'function_name' for add_function_override"));
	}

	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// 2. Resolve the function on the parent class
	UClass* ParentClass = Blueprint->ParentClass;
	ClaireonNameResolver::FNameResolveResult FuncResult;
	UFunction* TargetFunc = ParentClass
		? ClaireonNameResolver::ResolveFunctionName(ParentClass, FunctionName, FuncResult)
		: nullptr;

	if (!TargetFunc)
	{
		return MakeErrorResult(FuncResult.Error.IsEmpty()
				? FString::Printf(TEXT("Function '%s' not found: Blueprint has no parent class"), *FunctionName)
				: FuncResult.Error);
	}

	// 3. Validate FUNC_BlueprintEvent flag
	if (!TargetFunc->HasAnyFunctionFlags(FUNC_BlueprintEvent))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Function '%s' is not a BlueprintNativeEvent or BlueprintImplementableEvent"),
			*TargetFunc->GetName()));
	}

	// 4. Check for existing override via TWO mechanisms
	// 4a. Check for UK2Node_Event override in EventGraph
	UK2Node_Event* ExistingEventOverride = FBlueprintEditorUtils::FindOverrideForFunction(
		Blueprint, ParentClass, TargetFunc->GetFName());
	if (ExistingEventOverride)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Override for '%s' already exists as event node (GUID: %s)"),
			*TargetFunc->GetName(), *ExistingEventOverride->NodeGuid.ToString()));
	}

	// 4b. Check for existing function graph with the function's name
	FName FuncFName = TargetFunc->GetFName();
	for (UEdGraph* ExistingGraph : Blueprint->FunctionGraphs)
	{
		if (ExistingGraph && ExistingGraph->GetFName() == FuncFName)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Override for '%s' already exists as function graph"),
				*TargetFunc->GetName()));
		}
	}

	// 5. Branch on FUNC_Native
	bool bIsNativeEvent = TargetFunc->HasAnyFunctionFlags(FUNC_Native);

	if (bIsNativeEvent)
	{
		// === Native path: create a function graph ===

		// Create the graph
		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint,
			FuncFName,
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());

		if (!NewGraph)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Failed to create function graph for '%s'"), *TargetFunc->GetName()));
		}

		// Register it as a function graph
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(Blueprint, NewGraph, /*bIsUserCreated=*/true, /*SignatureFromClass=*/nullptr);

		// Get the auto-created entry node
		UK2Node_FunctionEntry* EntryNode = nullptr;
		{
			TArray<UK2Node_FunctionEntry*> EntryNodes;
			NewGraph->GetNodesOfClass(EntryNodes);
			if (EntryNodes.Num() > 0)
			{
				EntryNode = EntryNodes[0];
			}
		}

		if (!EntryNode)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Failed to find entry node in new function graph for '%s'"), *TargetFunc->GetName()));
		}

		// Bind entry node to parent function signature
		EntryNode->FunctionReference.SetExternalMember(TargetFunc->GetFName(), ParentClass);
		EntryNode->ReconstructNode();

		// Create/find result node
		UK2Node_FunctionResult* ResultNode =
			FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);
		if (ResultNode)
		{
			ResultNode->ReconstructNode();
		}

		// Mark blueprint as modified
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		// Switch session to the new function graph.
		// Capture the old graph name FIRST so we can push a correct history entry.
		const FString PreviousGraphName = Data->Cursor.GraphName;
		Data->Cursor.PushHistory(PreviousGraphName);

		Data->Graph = NewGraph;
		Data->Cursor.GraphName = NewGraph->GetName();
		Data->Cursor.FocusedNodeGuid = EntryNode->NodeGuid;

		// Set cursor to entry node's first output exec pin
		for (UEdGraphPin* Pin : EntryNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				Data->Cursor.FocusedPinName = Pin->PinName;
				Data->Cursor.FocusedPinDirection = Pin->Direction;
				break;
			}
		}

		Data->Cursor.LastOperationStatus = FString::Printf(
			TEXT("Created function override graph for '%s' (native event). Session graph switched to '%s'."),
			*TargetFunc->GetName(), *NewGraph->GetName());

		// Track affected nodes for response_mode="changed"
		Data->LastOperationAffectedNodes.Add(EntryNode->NodeGuid);
		if (ResultNode)
		{
			Data->LastOperationAffectedNodes.Add(ResultNode->NodeGuid);
		}
	}
	else
	{
		// === Implementable path: create UK2Node_Event in EventGraph ===

		UEdGraph* Graph = Data->Graph.Get();
		if (!Graph)
		{
			return MakeErrorResult(TEXT("Current graph is no longer valid"));
		}

		UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
		EventNode->EventReference.SetExternalMember(TargetFunc->GetFName(), ParentClass);
		EventNode->bOverrideFunction = true;

		// Place node at cursor viewport center
		EventNode->NodePosX = FMath::RoundToInt(Data->Cursor.ViewportCenter.X);
		EventNode->NodePosY = FMath::RoundToInt(Data->Cursor.ViewportCenter.Y);

		Graph->AddNode(EventNode, /*bUserAction=*/true, /*bSelectNewNode=*/false);
		EventNode->AllocateDefaultPins();
		EventNode->ReconstructNode();

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		// Update cursor
		Data->Cursor.PushHistory(Data->Cursor.GraphName);
		Data->Cursor.FocusedNodeGuid = EventNode->NodeGuid;

		for (UEdGraphPin* Pin : EventNode->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				Data->Cursor.FocusedPinName = Pin->PinName;
				Data->Cursor.FocusedPinDirection = Pin->Direction;
				break;
			}
		}

		Data->Cursor.LastOperationStatus = FString::Printf(
			TEXT("Created event override for '%s' (implementable event) in EventGraph"),
			*TargetFunc->GetName());

		Data->LastOperationAffectedNodes.Add(EventNode->NodeGuid);
	}

	// Include resolution note if applicable
	if (!FuncResult.ResolutionNote.IsEmpty())
	{
		Data->Cursor.LastOperationStatus += FString::Printf(TEXT(" [%s]"), *FuncResult.ResolutionNote);
	}

	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
