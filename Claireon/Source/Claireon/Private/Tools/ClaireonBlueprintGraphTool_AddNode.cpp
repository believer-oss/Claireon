// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
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
#include "K2Node_AsyncAction.h"
#include "Kismet/BlueprintAsyncActionBase.h"
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


FString ClaireonBlueprintGraphTool_AddNode::GetOperation() const { return TEXT("add_node"); }

TArray<FString> ClaireonBlueprintGraphTool_AddNode::GetSearchKeywords() const
{
    return {TEXT("bp"), TEXT("node"), TEXT("add"), TEXT("create"), TEXT("graph"), TEXT("auto_connect"), TEXT("cursor")};
}

FString ClaireonBlueprintGraphTool_AddNode::GetDescription() const
{
    return TEXT("Adds a node to the current session's graph (CallFunction, VariableGet/Set, control flow, macros, delegates, etc.). Pass auto_connect_from_cursor=true to route the new node's exec pin through the cursor pin when compatible. Most-common pitfall: forgetting to save every 1-3 nodes via bp_save, which loses progress on editor crash. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddNode::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("node_type"), TEXT("Node class alias or full name (e.g. CallFunction, AsyncAction, VariableGet, Branch, Sequence, Cast, MacroInstance, or a known macro shorthand like IsValid)."), true);
    Builder.AddString(TEXT("function_name"), TEXT("Function name for CallFunction."));
    Builder.AddString(TEXT("function_class"), TEXT("Class that owns the function (for CallFunction)."));
    Builder.AddString(TEXT("variable_name"), TEXT("Variable name for VariableGet/Set."));
    Builder.AddString(TEXT("macro_library"), TEXT("Macro library asset path for MacroInstance."));
    Builder.AddString(TEXT("macro_name"), TEXT("Macro name for MacroInstance."));
    Builder.AddString(TEXT("target_class"), TEXT("Class to cast/spawn (for Cast/SpawnActor)."));
    Builder.AddString(TEXT("struct_type"), TEXT("Struct type path for MakeStruct/BreakStruct."));
    Builder.AddString(TEXT("enum_type"), TEXT("Enum type path for SwitchEnum/ForEachElementInEnum."));
    Builder.AddString(TEXT("event_name"), TEXT("Event name for EventOverride/CustomEvent."));
    Builder.AddString(TEXT("comment_text"), TEXT("Comment text for Comment nodes."));
    Builder.AddString(TEXT("delegate_name"), TEXT("Delegate/event name for delegate nodes."));
    Builder.AddNumber(TEXT("position_x"), TEXT("Optional X coordinate; defaults to cursor."));
    Builder.AddNumber(TEXT("position_y"), TEXT("Optional Y coordinate; defaults to cursor."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_AddNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("add_node"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return CheckMutationAffectedNodes(TEXT("add_node"), Data, AddNode_Impl(SessionId, Data, Params));
}

FToolResult ClaireonBlueprintGraphTool_AddNode::AddNode_Impl(
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

	ClaireonMacroShorthand::ResolveIfShorthand(Params);
	ClaireonNodeTypeAlias::ResolveNodeTypeAlias(Params);

	// Get node_type
	FString NodeType;
	if (!Params->TryGetStringField(TEXT("node_type"), NodeType))
	{
		return MakeErrorResult(TEXT("Missing required field: node_type"));
	}

	// Get optional position
	FVector2D Position = Data->Cursor.ViewportCenter;
	const TSharedPtr<FJsonObject>* PositionObj = nullptr;
	if (Params->TryGetObjectField(TEXT("position"), PositionObj))
	{
		double X = 0.0, Y = 0.0;
		(*PositionObj)->TryGetNumberField(TEXT("x"), X);
		(*PositionObj)->TryGetNumberField(TEXT("y"), Y);
		Position = FVector2D(X, Y);
	}

	// Get optional auto_connect flag
	bool bAutoConnect = false;
	Params->TryGetBoolField(TEXT("auto_connect_from_cursor"), bAutoConnect);

	// Create node using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Blueprint Node")));
	Blueprint->Modify();
	Graph->Modify();

	UEdGraphNode* NewNode = nullptr;
	FString NodeDescription;
	bool bNodeAlreadyAdded = false; // Set by AssignDelegate which handles its own graph insertion

	// Helper lambda for BaseMCDelegate nodes (AddDelegate, RemoveDelegate, ClearDelegate, CallDelegate)
	TArray<FString> ResolutionWarnings;

	auto ResolveAndSetDelegate = [&](UK2Node_BaseMCDelegate* DelegateNode,
									 const FString& DelegateName, const FString& TargetClass) -> FString /*error or empty*/
	{
		UClass* OwnerClass = nullptr;
		bool bSelfContext = TargetClass.IsEmpty();

		if (bSelfContext)
		{
			OwnerClass = Blueprint->SkeletonGeneratedClass
				? Blueprint->SkeletonGeneratedClass
				: Blueprint->ParentClass;
		}
		else
		{
			ClaireonNameResolver::FNameResolveResult DelegateClassResult;
			OwnerClass = ClaireonNameResolver::ResolveClassName(TargetClass, nullptr, DelegateClassResult);
			if (!OwnerClass)
			{
				return DelegateClassResult.Error;
			}
			if (!DelegateClassResult.ResolutionNote.IsEmpty())
			{
				ResolutionWarnings.Add(DelegateClassResult.ResolutionNote);
			}
		}

		if (!OwnerClass)
		{
			return FString::Printf(TEXT("Could not determine owner class for delegate '%s'"), *DelegateName);
		}

		FMulticastDelegateProperty* DelegateProp = CastField<FMulticastDelegateProperty>(
			OwnerClass->FindPropertyByName(FName(*DelegateName)));

		if (!DelegateProp)
		{
			// Check if the property exists but is not a multicast delegate
			FProperty* Prop = OwnerClass->FindPropertyByName(FName(*DelegateName));
			if (Prop)
			{
				return FString::Printf(TEXT("Property '%s' on class '%s' is not a multicast delegate (actual type: %s)"),
					*DelegateName, *GetNameSafe(OwnerClass), *Prop->GetClass()->GetName());
			}
			return FString::Printf(TEXT("Multicast delegate '%s' not found on class '%s'"),
				*DelegateName, *GetNameSafe(OwnerClass));
		}

		DelegateNode->SetFromProperty(DelegateProp, bSelfContext, OwnerClass);
		return FString(); // success
	};

	// Create node based on type
	if (NodeType == TEXT("CallFunction"))
	{
		FString FunctionName, FunctionClass;
		if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
		{
			return MakeErrorResult(TEXT("Missing required field 'function_name' for CallFunction node"));
		}
		Params->TryGetStringField(TEXT("function_class"), FunctionClass);

		// Resolve the owning class (if supplied) first so we can look up the
		// actual UFunction and pick the right UK2Node_* subclass, mirroring
		// UBlueprintFunctionNodeSpawner::Create. Array functions
		// (MD_ArrayParam), DataTable functions (MD_DataTablePin), material
		// parameter collection functions, and commutative binary operators all
		// have specialized subclasses whose overrides (especially
		// NotifyPinConnectionListChanged) perform wildcard pin propagation and
		// other behaviour the plain UK2Node_CallFunction cannot.
		UClass* ResolvedOwnerClass = nullptr;
		if (!FunctionClass.IsEmpty())
		{
			ClaireonNameResolver::FNameResolveResult FuncClassResult;
			ResolvedOwnerClass = ClaireonNameResolver::ResolveClassName(FunctionClass, nullptr, FuncClassResult);
			if (ResolvedOwnerClass && !FuncClassResult.ResolutionNote.IsEmpty())
			{
				ResolutionWarnings.Add(FuncClassResult.ResolutionNote);
			}
		}

		UFunction* ResolvedFunction = nullptr;
		if (ResolvedOwnerClass)
		{
			ResolvedFunction = ResolvedOwnerClass->FindFunctionByName(FName(*FunctionName));
		}
		else if (Blueprint->SkeletonGeneratedClass)
		{
			ResolvedFunction = Blueprint->SkeletonGeneratedClass->FindFunctionByName(FName(*FunctionName));
		}

		// Pick the specialized K2 node subclass that matches the function's
		// metadata. Shared with ClaireonBlueprintNodeFactory::CreateNode so both
		// the incremental (Operation_AddNode) and batch (ApplyBlueprintGraph)
		// paths stay in lockstep. See
		// UBlueprintFunctionNodeSpawner::Create for the canonical order.
		UClass* NodeClass = ClaireonBlueprintHelpers::PickK2NodeClassForFunction(ResolvedFunction);

		if (NodeClass && NodeClass->IsChildOf(UK2Node_BaseAsyncTask::StaticClass()))
		{
			// AsyncAction branch: helper guarantees ResolvedFunction is a valid
			// UBlueprintAsyncActionBase factory (Fracture 01, conjunct 4). No
			// FunctionReference set; InitializeProxyFromFunction populates the
			// proxy fields directly.
			UK2Node_AsyncAction* AsyncNode = NewObject<UK2Node_AsyncAction>(Graph);
			AsyncNode->InitializeProxyFromFunction(ResolvedFunction);
			NewNode = AsyncNode;
			NodeDescription = FString::Printf(TEXT("AsyncAction: %s (%s)"),
				*FunctionName, *NodeClass->GetName());
		}
		else
		{
			UK2Node_CallFunction* CallFuncNode = NewObject<UK2Node_CallFunction>(Graph, NodeClass);

			if (ResolvedOwnerClass)
			{
				CallFuncNode->FunctionReference.SetExternalMember(FName(*FunctionName), ResolvedOwnerClass);
			}
			else
			{
				CallFuncNode->FunctionReference.SetSelfMember(FName(*FunctionName));
			}

			NewNode = CallFuncNode;
			NodeDescription = FString::Printf(TEXT("CallFunction: %s (%s)"),
				*FunctionName, *NodeClass->GetName());
		}
	}
	else if (NodeType == TEXT("AsyncAction"))
	{
		// Explicit AsyncAction surface: callers state intent directly instead of
		// relying on CallFunction helper-detection. Both paths construct the same
		// node; this one skips the helper-detection guard for callers who already
		// know they want an AsyncAction node. Mirrors the factory edit in
		// ClaireonBlueprintNodeFactory.cpp::CreateNode.
		FString FunctionName, FunctionClass;
		if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
		{
			return MakeErrorResult(TEXT("AsyncAction: missing required field 'function_name'"));
		}
		if (!Params->TryGetStringField(TEXT("function_class"), FunctionClass))
		{
			return MakeErrorResult(TEXT("AsyncAction: missing required field 'function_class' (the UClass that hosts the async factory UFUNCTION, e.g. '/Script/Engine.AsyncActionLoadPrimaryAsset')"));
		}

		ClaireonNameResolver::FNameResolveResult AsyncClassResult;
		UClass* OwnerClass = ClaireonNameResolver::ResolveClassName(FunctionClass, UBlueprintAsyncActionBase::StaticClass(), AsyncClassResult);
		if (!OwnerClass)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("AsyncAction: function_class '%s' could not be resolved to a UBlueprintAsyncActionBase-derived class. (%s)"),
				*FunctionClass, *AsyncClassResult.Error));
		}
		if (!AsyncClassResult.ResolutionNote.IsEmpty())
		{
			ResolutionWarnings.Add(AsyncClassResult.ResolutionNote);
		}

		UFunction* ResolvedFunction = OwnerClass->FindFunctionByName(FName(*FunctionName));
		if (!ResolvedFunction)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("AsyncAction: factory function '%s' not found on class '%s'."),
				*FunctionName, *OwnerClass->GetName()));
		}

		UK2Node_AsyncAction* AsyncNode = NewObject<UK2Node_AsyncAction>(Graph);
		AsyncNode->InitializeProxyFromFunction(ResolvedFunction);
		NewNode = AsyncNode;
		NodeDescription = FString::Printf(TEXT("AsyncAction: %s (%s)"), *FunctionName, *OwnerClass->GetName());
	}
	else if (NodeType == TEXT("VariableGet"))
	{
		FString VariableName;
		if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
		{
			return MakeErrorResult(TEXT("Missing required field 'variable_name' for VariableGet node"));
		}

		UK2Node_VariableGet* VarGetNode = NewObject<UK2Node_VariableGet>(Graph);
		VarGetNode->VariableReference.SetSelfMember(FName(*VariableName));
		NewNode = VarGetNode;
		NodeDescription = FString::Printf(TEXT("Get %s"), *VariableName);
	}
	else if (NodeType == TEXT("VariableSet"))
	{
		FString VariableName;
		if (!Params->TryGetStringField(TEXT("variable_name"), VariableName))
		{
			return MakeErrorResult(TEXT("Missing required field 'variable_name' for VariableSet node"));
		}

		UK2Node_VariableSet* VarSetNode = NewObject<UK2Node_VariableSet>(Graph);
		VarSetNode->VariableReference.SetSelfMember(FName(*VariableName));
		NewNode = VarSetNode;
		NodeDescription = FString::Printf(TEXT("Set %s"), *VariableName);
	}
	else if (NodeType == TEXT("Branch"))
	{
		UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(Graph);
		NewNode = BranchNode;
		NodeDescription = TEXT("Branch");
	}
	else if (NodeType == TEXT("Sequence"))
	{
		UK2Node_ExecutionSequence* SeqNode = NewObject<UK2Node_ExecutionSequence>(Graph);
		NewNode = SeqNode;
		NodeDescription = TEXT("Sequence");
	}
	else if (NodeType == TEXT("Cast"))
	{
		FString TargetClass;
		if (!Params->TryGetStringField(TEXT("target_class"), TargetClass))
		{
			return MakeErrorResult(TEXT("Missing required field 'target_class' for Cast node"));
		}

		ClaireonNameResolver::FNameResolveResult CastClassResult;
		UClass* CastClass = ClaireonNameResolver::ResolveClassName(TargetClass, nullptr, CastClassResult);
		if (!CastClass)
		{
			return MakeErrorResult(CastClassResult.Error);
		}
		if (!CastClassResult.ResolutionNote.IsEmpty())
		{
			ResolutionWarnings.Add(CastClassResult.ResolutionNote);
		}

		UK2Node_DynamicCast* CastNode = NewObject<UK2Node_DynamicCast>(Graph);
		CastNode->TargetType = CastClass;
		NewNode = CastNode;
		NodeDescription = FString::Printf(TEXT("Cast to %s"), *TargetClass);
	}
	else if (NodeType == TEXT("SpawnActor"))
	{
		FString ActorClass;
		if (!Params->TryGetStringField(TEXT("actor_class"), ActorClass))
		{
			return MakeErrorResult(TEXT("Missing required field 'actor_class' for SpawnActor node"));
		}

		ClaireonNameResolver::FNameResolveResult SpawnClassResult;
		UClass* SpawnClass = ClaireonNameResolver::ResolveClassName(ActorClass, AActor::StaticClass(), SpawnClassResult);
		if (!SpawnClass)
		{
			return MakeErrorResult(SpawnClassResult.Error);
		}
		if (!SpawnClassResult.ResolutionNote.IsEmpty())
		{
			ResolutionWarnings.Add(SpawnClassResult.ResolutionNote);
		}

		UK2Node_SpawnActorFromClass* SpawnNode = NewObject<UK2Node_SpawnActorFromClass>(Graph);
		NewNode = SpawnNode;
		NodeDescription = FString::Printf(TEXT("Spawn %s"), *ActorClass);
	}
	else if (NodeType == TEXT("CustomEvent"))
	{
		FString EventName;
		if (!Params->TryGetStringField(TEXT("event_name"), EventName))
		{
			return MakeErrorResult(TEXT("Missing required field 'event_name' for CustomEvent node"));
		}

		UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(Graph);
		EventNode->CustomFunctionName = FName(*EventName);
		NewNode = EventNode;
		NodeDescription = FString::Printf(TEXT("Custom Event: %s"), *EventName);
	}
	else if (NodeType == TEXT("Knot"))
	{
		UK2Node_Knot* KnotNode = NewObject<UK2Node_Knot>(Graph);
		NewNode = KnotNode;
		NodeDescription = TEXT("Reroute Node");
	}
	else if (NodeType == TEXT("Comment"))
	{
		FString CommentText;
		if (!Params->TryGetStringField(TEXT("comment_text"), CommentText))
		{
			CommentText = TEXT("Comment");
		}

		UEdGraphNode_Comment* CommentNode = NewObject<UEdGraphNode_Comment>(Graph);
		CommentNode->NodeComment = CommentText;
		NewNode = CommentNode;
		NodeDescription = FString::Printf(TEXT("Comment: %s"), *CommentText);
	}
	else if (NodeType == TEXT("Select"))
	{
		UK2Node_Select* SelectNode = NewObject<UK2Node_Select>(Graph);
		NewNode = SelectNode;
		NodeDescription = TEXT("Select");
	}
	else if (NodeType == TEXT("MakeArray"))
	{
		UK2Node_MakeArray* MakeArrayNode = NewObject<UK2Node_MakeArray>(Graph);
		NewNode = MakeArrayNode;
		NodeDescription = TEXT("Make Array");
	}
	else if (NodeType == TEXT("MakeSet"))
	{
		UK2Node_MakeSet* MakeSetNode = NewObject<UK2Node_MakeSet>(Graph);
		NewNode = MakeSetNode;
		NodeDescription = TEXT("Make Set");
	}
	else if (NodeType == TEXT("MakeMap"))
	{
		UK2Node_MakeMap* MakeMapNode = NewObject<UK2Node_MakeMap>(Graph);
		NewNode = MakeMapNode;
		NodeDescription = TEXT("Make Map");
	}
	else if (NodeType == TEXT("GetArrayItem"))
	{
		UK2Node_GetArrayItem* GetArrayNode = NewObject<UK2Node_GetArrayItem>(Graph);
		NewNode = GetArrayNode;
		NodeDescription = TEXT("Get Array Item");
	}
	else if (NodeType == TEXT("MakeStruct"))
	{
		FString StructType;
		if (!Params->TryGetStringField(TEXT("struct_type"), StructType))
		{
			return MakeErrorResult(TEXT("Missing required field 'struct_type' for MakeStruct node"));
		}

		ClaireonNameResolver::FNameResolveResult MakeStructResult;
		UScriptStruct* Struct = ClaireonNameResolver::ResolveStructName(StructType, MakeStructResult);
		if (!Struct)
		{
			return MakeErrorResult(MakeStructResult.Error);
		}
		if (!MakeStructResult.ResolutionNote.IsEmpty())
		{
			ResolutionWarnings.Add(MakeStructResult.ResolutionNote);
		}

		UK2Node_MakeStruct* MakeStructNode = NewObject<UK2Node_MakeStruct>(Graph);
		MakeStructNode->StructType = Struct;
		NewNode = MakeStructNode;
		NodeDescription = FString::Printf(TEXT("Make %s"), *StructType);
	}
	else if (NodeType == TEXT("BreakStruct"))
	{
		FString StructType;
		if (!Params->TryGetStringField(TEXT("struct_type"), StructType))
		{
			return MakeErrorResult(TEXT("Missing required field 'struct_type' for BreakStruct node"));
		}

		ClaireonNameResolver::FNameResolveResult BreakStructResult;
		UScriptStruct* Struct = ClaireonNameResolver::ResolveStructName(StructType, BreakStructResult);
		if (!Struct)
		{
			return MakeErrorResult(BreakStructResult.Error);
		}
		if (!BreakStructResult.ResolutionNote.IsEmpty())
		{
			ResolutionWarnings.Add(BreakStructResult.ResolutionNote);
		}

		UK2Node_BreakStruct* BreakStructNode = NewObject<UK2Node_BreakStruct>(Graph);
		BreakStructNode->StructType = Struct;
		NewNode = BreakStructNode;
		NodeDescription = FString::Printf(TEXT("Break %s"), *StructType);
	}
	// --- Switch node types ---
	else if (NodeType == TEXT("SwitchInteger"))
	{
		UK2Node_SwitchInteger* SwitchNode = NewObject<UK2Node_SwitchInteger>(Graph);
		NewNode = SwitchNode;
		NodeDescription = TEXT("Switch on Int");
	}
	else if (NodeType == TEXT("SwitchString"))
	{
		UK2Node_SwitchString* SwitchNode = NewObject<UK2Node_SwitchString>(Graph);
		NewNode = SwitchNode;
		NodeDescription = TEXT("Switch on String");
	}
	else if (NodeType == TEXT("SwitchName"))
	{
		UK2Node_SwitchName* SwitchNode = NewObject<UK2Node_SwitchName>(Graph);
		NewNode = SwitchNode;
		NodeDescription = TEXT("Switch on Name");
	}
	else if (NodeType == TEXT("SwitchEnum"))
	{
		FString EnumType;
		if (!Params->TryGetStringField(TEXT("enum_type"), EnumType))
		{
			return MakeErrorResult(TEXT("Missing required field 'enum_type' for SwitchEnum node"));
		}

		ClaireonNameResolver::FNameResolveResult SwitchEnumResult;
		UEnum* Enum = ClaireonNameResolver::ResolveEnumName(EnumType, SwitchEnumResult);
		if (!Enum)
		{
			return MakeErrorResult(SwitchEnumResult.Error);
		}
		if (!SwitchEnumResult.ResolutionNote.IsEmpty())
		{
			ResolutionWarnings.Add(SwitchEnumResult.ResolutionNote);
		}

		UK2Node_SwitchEnum* SwitchNode = NewObject<UK2Node_SwitchEnum>(Graph);
		// SetEnum is not exported, so replicate its logic: set Enum + populate EnumEntries/EnumFriendlyNames
		SwitchNode->Enum = Enum;
		SwitchNode->EnumEntries.Empty();
		SwitchNode->EnumFriendlyNames.Empty();
		for (int32 EnumIdx = 0; EnumIdx < Enum->NumEnums() - 1; ++EnumIdx)
		{
			bool bShouldBeHidden = Enum->HasMetaData(TEXT("Hidden"), EnumIdx) || Enum->HasMetaData(TEXT("Spacer"), EnumIdx);
			if (!bShouldBeHidden)
			{
				SwitchNode->EnumEntries.Add(FName(*Enum->GetNameStringByIndex(EnumIdx)));
				SwitchNode->EnumFriendlyNames.Add(Enum->GetDisplayNameTextByIndex(EnumIdx));
			}
		}
		NewNode = SwitchNode;
		NodeDescription = FString::Printf(TEXT("Switch on %s"), *EnumType);
	}
	// --- Enum iteration ---
	else if (NodeType == TEXT("ForEachElementInEnum"))
	{
		FString EnumType;
		if (!Params->TryGetStringField(TEXT("enum_type"), EnumType))
		{
			return MakeErrorResult(TEXT("Missing required field 'enum_type' for ForEachElementInEnum node"));
		}

		ClaireonNameResolver::FNameResolveResult ForEachEnumResult;
		UEnum* Enum = ClaireonNameResolver::ResolveEnumName(EnumType, ForEachEnumResult);
		if (!Enum)
		{
			return MakeErrorResult(ForEachEnumResult.Error);
		}
		if (!ForEachEnumResult.ResolutionNote.IsEmpty())
		{
			ResolutionWarnings.Add(ForEachEnumResult.ResolutionNote);
		}

		UK2Node_ForEachElementInEnum* ForEachNode = NewObject<UK2Node_ForEachElementInEnum>(Graph);
		ForEachNode->Enum = Enum;
		NewNode = ForEachNode;
		NodeDescription = FString::Printf(TEXT("For Each %s"), *EnumType);
	}
	// --- DoOnceMultiInput ---
	else if (NodeType == TEXT("DoOnceMultiInput"))
	{
		UK2Node_DoOnceMultiInput* DoOnceNode = NewObject<UK2Node_DoOnceMultiInput>(Graph);
		NewNode = DoOnceNode;
		NodeDescription = TEXT("Do Once (Multi Input)");
	}
	// --- Macro nodes (StandardMacros library aliases + generic Macro / MacroInstance types) ---
	else if (NodeType == TEXT("Macro") || NodeType == TEXT("MacroInstance")
		|| NodeType == TEXT("ForEachLoop") || NodeType == TEXT("ForEachLoopWithBreak")
		|| NodeType == TEXT("ForLoop") || NodeType == TEXT("ForLoopWithBreak") || NodeType == TEXT("WhileLoop")
		|| NodeType == TEXT("DoOnce") || NodeType == TEXT("DoN") || NodeType == TEXT("FlipFlop")
		|| NodeType == TEXT("Gate") || NodeType == TEXT("MultiGate") || NodeType == TEXT("IsValid"))
	{
		// Determine macro name and library path
		FString MacroName;
		FString MacroLibraryPath = TEXT("/Engine/EditorBlueprintResources/StandardMacros");

		if (NodeType == TEXT("Macro") || NodeType == TEXT("MacroInstance"))
		{
			if (!Params->TryGetStringField(TEXT("macro_name"), MacroName))
			{
				return MakeErrorResult(TEXT("Missing required field 'macro_name' for Macro node type"));
			}
			FString CustomLibPath;
			if (Params->TryGetStringField(TEXT("macro_library"), CustomLibPath)
				|| Params->TryGetStringField(TEXT("macro_library_path"), CustomLibPath))
			{
				MacroLibraryPath = CustomLibPath;
			}
		}
		else
		{
			// Named alias -- NodeType IS the macro name
			MacroName = NodeType;
		}

		// Load the macro library blueprint
		UBlueprint* MacroLib = LoadObject<UBlueprint>(nullptr, *MacroLibraryPath);
		if (!MacroLib)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to load macro library: %s"), *MacroLibraryPath));
		}

		// Find the macro graph by name
		UEdGraph* MacroGraph = nullptr;
		for (UEdGraph* MacroGraphCandidate : MacroLib->MacroGraphs)
		{
			if (MacroGraphCandidate && MacroGraphCandidate->GetName() == MacroName)
			{
				MacroGraph = MacroGraphCandidate;
				break;
			}
		}

		if (!MacroGraph)
		{
			// List available macros for discoverability
			TArray<FString> AvailableMacros;
			for (UEdGraph* G : MacroLib->MacroGraphs)
			{
				if (G)
				{
					AvailableMacros.Add(G->GetName());
				}
			}
			return MakeErrorResult(FString::Printf(TEXT("Macro '%s' not found in %s. Available: %s"),
				*MacroName, *MacroLibraryPath, *FString::Join(AvailableMacros, TEXT(", "))));
		}

		UK2Node_MacroInstance* MacroNode = NewObject<UK2Node_MacroInstance>(Graph);
		MacroNode->SetMacroGraph(MacroGraph);
		NewNode = MacroNode;
		NodeDescription = FString::Printf(TEXT("Macro: %s"), *MacroName);
	}
	else if (NodeType == TEXT("Generic"))
	{
		// Generic node type - accepts any K2Node class name
		FString ClassName;
		if (!Params->TryGetStringField(TEXT("class_name"), ClassName))
		{
			return MakeErrorResult(TEXT("Missing required field 'class_name' for Generic node type. Specify the K2Node class name (e.g., 'K2Node_AddPinInterface')"));
		}

		// Find the node class using fuzzy resolution (handles U prefix, K2Node_ prefix, etc.)
		ClaireonNameResolver::FNameResolveResult NodeClassResult;
		UClass* NodeClass = ClaireonNameResolver::ResolveClassName(ClassName, UK2Node::StaticClass(), NodeClassResult);
		if (!NodeClass)
		{
			return MakeErrorResult(NodeClassResult.Error);
		}
		if (!NodeClassResult.ResolutionNote.IsEmpty())
		{
			ResolutionWarnings.Add(NodeClassResult.ResolutionNote);
		}

		if (!NodeClass->IsChildOf(UEdGraphNode::StaticClass()))
		{
			return MakeErrorResult(FString::Printf(TEXT("Class '%s' is not a graph node class"), *ClassName));
		}

		NewNode = NewObject<UEdGraphNode>(Graph, NodeClass);

		// Set node_properties via reflection before AllocateDefaultPins
		const TSharedPtr<FJsonObject>* NodePropsPtr = nullptr;
		if (Params->TryGetObjectField(TEXT("node_properties"), NodePropsPtr) && NodePropsPtr)
		{
			for (auto& Pair : (*NodePropsPtr)->Values)
			{
				FProperty* Prop = NewNode->GetClass()->FindPropertyByName(FName(*Pair.Key));
				if (!Prop)
				{
					UE_LOG(LogClaireon, Warning, TEXT("node_properties: Property '%s' not found on %s"), *Pair.Key, *ClassName);
					continue;
				}

				void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(NewNode);

				if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
				{
					bool bVal = false;
					Pair.Value->TryGetBool(bVal);
					BoolProp->SetPropertyValue(ValuePtr, bVal);
				}
				else if (FIntProperty* IntProp = CastField<FIntProperty>(Prop))
				{
					double NumVal = 0;
					Pair.Value->TryGetNumber(NumVal);
					IntProp->SetPropertyValue(ValuePtr, static_cast<int32>(NumVal));
				}
				else if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
				{
					double NumVal = 0;
					Pair.Value->TryGetNumber(NumVal);
					FloatProp->SetPropertyValue(ValuePtr, static_cast<float>(NumVal));
				}
				else if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
				{
					double NumVal = 0;
					Pair.Value->TryGetNumber(NumVal);
					DoubleProp->SetPropertyValue(ValuePtr, NumVal);
				}
				else if (FStrProperty* StrProp = CastField<FStrProperty>(Prop))
				{
					FString StrVal;
					Pair.Value->TryGetString(StrVal);
					StrProp->SetPropertyValue(ValuePtr, StrVal);
				}
				else if (FNameProperty* NameProp = CastField<FNameProperty>(Prop))
				{
					FString StrVal;
					Pair.Value->TryGetString(StrVal);
					NameProp->SetPropertyValue(ValuePtr, FName(*StrVal));
				}
				else if (FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
				{
					FString StrVal;
					Pair.Value->TryGetString(StrVal);
					if (ObjProp->PropertyClass->IsChildOf(UClass::StaticClass()))
					{
						ClaireonNameResolver::FNameResolveResult PropClassResult;
						UClass* FoundClass = ClaireonNameResolver::ResolveClassName(StrVal, nullptr, PropClassResult);
						if (FoundClass)
						{
							ObjProp->SetObjectPropertyValue(ValuePtr, FoundClass);
							if (!PropClassResult.ResolutionNote.IsEmpty())
							{
								ResolutionWarnings.Add(PropClassResult.ResolutionNote);
							}
						}
					}
					else if (ObjProp->PropertyClass->IsChildOf(UEnum::StaticClass()))
					{
						ClaireonNameResolver::FNameResolveResult PropEnumResult;
						UEnum* FoundEnum = ClaireonNameResolver::ResolveEnumName(StrVal, PropEnumResult);
						if (FoundEnum)
						{
							ObjProp->SetObjectPropertyValue(ValuePtr, FoundEnum);
							if (!PropEnumResult.ResolutionNote.IsEmpty())
							{
								ResolutionWarnings.Add(PropEnumResult.ResolutionNote);
							}
						}
					}
					else if (ObjProp->PropertyClass->IsChildOf(UScriptStruct::StaticClass()))
					{
						ClaireonNameResolver::FNameResolveResult PropStructResult;
						UScriptStruct* FoundStruct = ClaireonNameResolver::ResolveStructName(StrVal, PropStructResult);
						if (FoundStruct)
						{
							ObjProp->SetObjectPropertyValue(ValuePtr, FoundStruct);
							if (!PropStructResult.ResolutionNote.IsEmpty())
							{
								ResolutionWarnings.Add(PropStructResult.ResolutionNote);
							}
						}
					}
					else
					{
						UE_LOG(LogClaireon, Warning, TEXT("node_properties: Unsupported object property type for '%s'"), *Pair.Key);
					}
				}
				else
				{
					UE_LOG(LogClaireon, Warning, TEXT("node_properties: Unsupported property type for '%s'"), *Pair.Key);
				}
			}
		}

		// Loud-failure guard for Generic + UK2Node_BaseAsyncTask subclasses (e.g.
		// K2Node_AsyncAction, K2Node_LatentAbilityCall) constructed without the
		// proxy bag. The engine's AllocateDefaultPins synthesizes the
		// BlueprintAssignable delegate exec pins from ProxyFactoryClass +
		// ProxyFactoryFunctionName + ProxyClass; if any are unset (via
		// node_properties), the result is the inert "Async Task: Missing Function"
		// stub. The three proxy fields are protected on UK2Node_BaseAsyncTask
		// with no public accessors, so we read them via reflection. Mirrors the
		// factory guard in ClaireonBlueprintNodeFactory.cpp::CreateNode.
		if (NewNode && NewNode->IsA<UK2Node_BaseAsyncTask>())
		{
			auto ReadObjProp = [&](const TCHAR* PropName) -> UClass*
			{
				if (const FObjectPropertyBase* P = CastField<FObjectPropertyBase>(
					NewNode->GetClass()->FindPropertyByName(PropName)))
				{
					return Cast<UClass>(P->GetObjectPropertyValue_InContainer(NewNode));
				}
				return nullptr;
			};
			auto ReadNameProp = [&](const TCHAR* PropName) -> FName
			{
				if (const FNameProperty* P = CastField<FNameProperty>(
					NewNode->GetClass()->FindPropertyByName(PropName)))
				{
					return P->GetPropertyValue_InContainer(NewNode);
				}
				return NAME_None;
			};

			UClass* PFC      = ReadObjProp(TEXT("ProxyFactoryClass"));
			UClass* PC       = ReadObjProp(TEXT("ProxyClass"));
			const FName PFF  = ReadNameProp(TEXT("ProxyFactoryFunctionName"));

			if (PFC == nullptr || PC == nullptr || PFF.IsNone())
			{
				return MakeErrorResult(FString::Printf(
					TEXT("Generic '%s' was created without proxy fields populated -- ")
					TEXT("the node would render as 'Async Task: Missing Function' with no delegate pins. ")
					TEXT("Use node_type='AsyncAction' with function_name and function_class (recommended). ")
					TEXT("As a fallback, supply node_properties containing 'ProxyFactoryFunctionName', 'ProxyFactoryClass', and 'ProxyClass'."),
					*NewNode->GetClass()->GetName()));
			}
		}

		NodeDescription = FString::Printf(TEXT("Generic: %s"), *ClassName);
	}
	else if (NodeType == TEXT("EventOverride"))
	{
		FString FunctionName;
		if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
		{
			return MakeErrorResult(TEXT("Missing required field 'function_name' for EventOverride node"));
		}

		UClass* ParentClass = Blueprint->ParentClass;
		ClaireonNameResolver::FNameResolveResult EventFuncResult;
		UFunction* TargetFunc = ParentClass
			? ClaireonNameResolver::ResolveFunctionName(ParentClass, FunctionName, EventFuncResult)
			: nullptr;

		if (!TargetFunc)
		{
			return MakeErrorResult(EventFuncResult.Error.IsEmpty()
					? FString::Printf(TEXT("Function '%s' not found: Blueprint has no parent class"), *FunctionName)
					: EventFuncResult.Error);
		}
		if (!EventFuncResult.ResolutionNote.IsEmpty())
		{
			ResolutionWarnings.Add(EventFuncResult.ResolutionNote);
		}

		if (!TargetFunc->HasAnyFunctionFlags(FUNC_BlueprintEvent))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Function '%s' is not a BlueprintNativeEvent or BlueprintImplementableEvent"),
				*TargetFunc->GetName()));
		}

		// Diagnostic: recommend add_function_override for BlueprintNativeEvent functions
		if (TargetFunc->HasAnyFunctionFlags(FUNC_Native))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Function '%s' is a BlueprintNativeEvent. Use the add_function_override operation instead of EventOverride node_type."),
				*TargetFunc->GetName()));
		}

		// Check for existing override
		UK2Node_Event* ExistingOverride = FBlueprintEditorUtils::FindOverrideForFunction(
			Blueprint, ParentClass, TargetFunc->GetFName());
		if (ExistingOverride)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Override for '%s' already exists (node GUID: %s)"),
				*TargetFunc->GetName(), *ExistingOverride->NodeGuid.ToString()));
		}

		UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
		EventNode->EventReference.SetExternalMember(TargetFunc->GetFName(), ParentClass);
		EventNode->bOverrideFunction = true;

		NewNode = EventNode;
		NodeDescription = FString::Printf(TEXT("Event Override: %s"), *TargetFunc->GetName());
	}
	else if (NodeType == TEXT("FunctionEntry"))
	{
		// Find-or-return on the function's entry node (function graphs always have exactly one).
		// Creation is never attempted; the entry node is seeded when the function graph is created.
		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		const EGraphType GraphType = K2Schema ? K2Schema->GetGraphType(Graph) : GT_MAX;
		if (GraphType != GT_Function)
		{
			return MakeErrorResult(TEXT("FunctionEntry can only be added to a function graph"));
		}

		UK2Node_FunctionEntry* EntryNode = nullptr;
		for (UEdGraphNode* ExistingNode : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(ExistingNode))
			{
				EntryNode = AsEntry;
				break;
			}
		}

		if (!EntryNode)
		{
			return MakeErrorResult(TEXT("Function graph has no entry node; blueprint may be corrupt"));
		}

		// Move cursor to the existing node and return state. Do not create anything.
		Data->Cursor.PushHistory(Data->Cursor.GraphName);
		Data->Cursor.FocusedNodeGuid = EntryNode->NodeGuid;
		if (UEdGraphPin* FirstOutputPin = ClaireonBlueprintHelpers::GetFirstOutputPin(EntryNode))
		{
			Data->Cursor.FocusedPinName = FirstOutputPin->PinName;
			Data->Cursor.FocusedPinDirection = FirstOutputPin->Direction;
		}
		Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Found existing FunctionEntry: %s"), *EntryNode->NodeGuid.ToString());
		Data->LastOperationAffectedNodes.Add(EntryNode->NodeGuid);

		FToolResult EntryResult = BuildStateResponse(SessionId, Data);
		EntryResult.Warnings.Append(ResolutionWarnings);
		return EntryResult;
	}
	else if (NodeType == TEXT("FunctionResult"))
	{
		// Find-or-create via engine helper (precedent: line 6257 FindOrCreateFunctionResultNode).
		// The helper takes UK2Node_FunctionEntry*, NOT UEdGraph*, so locate the entry node first.
		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		const EGraphType GraphType = K2Schema ? K2Schema->GetGraphType(Graph) : GT_MAX;
		if (GraphType != GT_Function)
		{
			return MakeErrorResult(TEXT("FunctionResult can only be added to a function graph"));
		}

		UK2Node_FunctionEntry* EntryNode = nullptr;
		for (UEdGraphNode* ExistingNode : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(ExistingNode))
			{
				EntryNode = AsEntry;
				break;
			}
		}
		if (!EntryNode)
		{
			return MakeErrorResult(TEXT("Function graph has no entry node; cannot add return node"));
		}

		UK2Node_FunctionResult* ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);
		if (!ResultNode)
		{
			return MakeErrorResult(TEXT("FindOrCreateFunctionResultNode returned null"));
		}
		ResultNode->ReconstructNode();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

		Data->Cursor.PushHistory(Data->Cursor.GraphName);
		Data->Cursor.FocusedNodeGuid = ResultNode->NodeGuid;
		if (UEdGraphPin* FirstOutputPin = ClaireonBlueprintHelpers::GetFirstOutputPin(ResultNode))
		{
			Data->Cursor.FocusedPinName = FirstOutputPin->PinName;
			Data->Cursor.FocusedPinDirection = FirstOutputPin->Direction;
		}
		Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Find-or-create FunctionResult: %s"), *ResultNode->NodeGuid.ToString());
		Data->LastOperationAffectedNodes.Add(ResultNode->NodeGuid);

		FToolResult ResultResp = BuildStateResponse(SessionId, Data);
		ResultResp.Warnings.Append(ResolutionWarnings);
		return ResultResp;
	}
	else if (NodeType == TEXT("Tunnel"))
	{
		// Find-or-return on the macro graph's two tunnel nodes (input + output).
		// Creation-on-demand is explicitly out of scope; missing tunnels -> structured error.
		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		const EGraphType GraphType = K2Schema ? K2Schema->GetGraphType(Graph) : GT_MAX;
		if (GraphType != GT_Macro)
		{
			return MakeErrorResult(TEXT("Tunnel can only be added to a macro graph"));
		}

		TArray<UK2Node_Tunnel*> TunnelNodes;
		for (UEdGraphNode* ExistingNode : Graph->Nodes)
		{
			if (UK2Node_Tunnel* AsTunnel = Cast<UK2Node_Tunnel>(ExistingNode))
			{
				TunnelNodes.Add(AsTunnel);
			}
		}

		if (TunnelNodes.Num() != 2)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Macro graph is missing tunnel nodes (found %d; expected 2); macro may be corrupt"),
				TunnelNodes.Num()));
		}

		// Move cursor to the first tunnel and attach both GUIDs to the response state.
		Data->Cursor.PushHistory(Data->Cursor.GraphName);
		Data->Cursor.FocusedNodeGuid = TunnelNodes[0]->NodeGuid;
		if (UEdGraphPin* FirstOutputPin = ClaireonBlueprintHelpers::GetFirstOutputPin(TunnelNodes[0]))
		{
			Data->Cursor.FocusedPinName = FirstOutputPin->PinName;
			Data->Cursor.FocusedPinDirection = FirstOutputPin->Direction;
		}
		Data->Cursor.LastOperationStatus = FString::Printf(
			TEXT("Found 2 tunnel nodes: %s, %s"),
			*TunnelNodes[0]->NodeGuid.ToString(), *TunnelNodes[1]->NodeGuid.ToString());
		Data->LastOperationAffectedNodes.Add(TunnelNodes[0]->NodeGuid);
		Data->LastOperationAffectedNodes.Add(TunnelNodes[1]->NodeGuid);

		FToolResult TunnelResult = BuildStateResponse(SessionId, Data);
		TunnelResult.Warnings.Append(ResolutionWarnings);
		return TunnelResult;
	}
	else if (NodeType == TEXT("CallParentFunction"))
	{
		FString FunctionName;
		if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
		{
			return MakeErrorResult(TEXT("Missing required field 'function_name' for CallParentFunction node"));
		}

		UClass* ParentClass = Blueprint->ParentClass;
		ClaireonNameResolver::FNameResolveResult ParentFuncResult;
		UFunction* TargetFunc = ParentClass
			? ClaireonNameResolver::ResolveFunctionName(ParentClass, FunctionName, ParentFuncResult)
			: nullptr;

		if (!TargetFunc)
		{
			return MakeErrorResult(ParentFuncResult.Error.IsEmpty()
					? FString::Printf(TEXT("Function '%s' not found: Blueprint has no parent class"), *FunctionName)
					: ParentFuncResult.Error);
		}
		if (!ParentFuncResult.ResolutionNote.IsEmpty())
		{
			ResolutionWarnings.Add(ParentFuncResult.ResolutionNote);
		}

		UK2Node_CallParentFunction* ParentCallNode =
			NewObject<UK2Node_CallParentFunction>(Graph);
		ParentCallNode->SetFromFunction(TargetFunc);

		NewNode = ParentCallNode;
		NodeDescription = FString::Printf(TEXT("Call Parent: %s"), *TargetFunc->GetName());
	}
	else if (NodeType == TEXT("Timeline"))
	{
		FString TimelineName;
		if (!Params->TryGetStringField(TEXT("timeline_name"), TimelineName))
		{
			return MakeErrorResult(TEXT("Missing required field 'timeline_name' for Timeline node"));
		}

		// Check for duplicate using canonical lookup (handles _Template naming)
		if (Blueprint->FindTimelineTemplateByVariableName(FName(*TimelineName)))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Timeline '%s' already exists in this Blueprint"), *TimelineName));
		}

		// Create the UK2Node_Timeline
		UK2Node_Timeline* TimelineNode = NewObject<UK2Node_Timeline>(Graph);
		TimelineNode->TimelineName = FName(*TimelineName);

		// Use engine utility to create UTimelineTemplate with correct naming,
		// Outer (GeneratedClass), RF_Transactional, and child BP validation
		UTimelineTemplate* TimelineTemplate =
			FBlueprintEditorUtils::AddNewTimeline(Blueprint, FName(*TimelineName));

		if (!TimelineTemplate)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Failed to create timeline '%s' -- Blueprint may not support timelines"),
				*TimelineName));
		}

		// Synchronize TimelineGuid so copy/paste works correctly
		TimelineNode->TimelineGuid = TimelineTemplate->TimelineGuid;

		bool bAutoplay = false;
		Params->TryGetBoolField(TEXT("autoplay"), bAutoplay);
		TimelineTemplate->bAutoPlay = bAutoplay;

		bool bLoop = false;
		Params->TryGetBoolField(TEXT("loop"), bLoop);
		TimelineTemplate->bLoop = bLoop;

		double MaxKeyTime = 0.0;

		// --- Add float tracks ---
		const TArray<TSharedPtr<FJsonValue>>* FloatTracksArray = nullptr;
		if (Params->TryGetArrayField(TEXT("float_tracks"), FloatTracksArray))
		{
			for (const auto& TrackVal : *FloatTracksArray)
			{
				const TSharedPtr<FJsonObject>& TrackObj = TrackVal->AsObject();
				if (!TrackObj)
					continue;

				FString TrackName;
				TrackObj->TryGetStringField(TEXT("track_name"), TrackName);

				FTTFloatTrack FloatTrack;
				FloatTrack.SetTrackName(FName(*TrackName), TimelineTemplate);

				// Create UCurveFloat UObject for the track's curve data
				FName CurveName = *FString::Printf(TEXT("%s_%s_Curve"),
					*TimelineName, *TrackName);
				UCurveFloat* CurveFloat = NewObject<UCurveFloat>(
					Blueprint->GeneratedClass, CurveName);
				FloatTrack.CurveFloat = CurveFloat;

				// Populate curve keys on CurveFloat->FloatCurve (the actual FRichCurve)
				FString Interp = TEXT("linear");
				TrackObj->TryGetStringField(TEXT("interpolation"), Interp);

				const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
				if (TrackObj->TryGetArrayField(TEXT("keys"), KeysArray))
				{
					for (const auto& KeyVal : *KeysArray)
					{
						const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
						if (!KeyObj)
							continue;

						double Time = 0.0, Value = 0.0;
						KeyObj->TryGetNumberField(TEXT("time"), Time);
						KeyObj->TryGetNumberField(TEXT("value"), Value);

						FKeyHandle Handle = CurveFloat->FloatCurve.AddKey(
							static_cast<float>(Time), static_cast<float>(Value));

						if (Interp == TEXT("constant"))
							CurveFloat->FloatCurve.SetKeyInterpMode(
								Handle, ERichCurveInterpMode::RCIM_Constant);
						else if (Interp == TEXT("cubic"))
							CurveFloat->FloatCurve.SetKeyInterpMode(
								Handle, ERichCurveInterpMode::RCIM_Cubic);
						else
							CurveFloat->FloatCurve.SetKeyInterpMode(
								Handle, ERichCurveInterpMode::RCIM_Linear);

						MaxKeyTime = FMath::Max(MaxKeyTime, Time);
					}
				}

				TimelineTemplate->FloatTracks.Add(FloatTrack);
				TimelineTemplate->AddDisplayTrack(
					FTTTrackId(FTTTrackBase::TT_FloatInterp,
						TimelineTemplate->FloatTracks.Num() - 1));
			}
		}

		// --- Add vector tracks ---
		const TArray<TSharedPtr<FJsonValue>>* VectorTracksArray = nullptr;
		if (Params->TryGetArrayField(TEXT("vector_tracks"), VectorTracksArray))
		{
			for (const auto& TrackVal : *VectorTracksArray)
			{
				const TSharedPtr<FJsonObject>& TrackObj = TrackVal->AsObject();
				if (!TrackObj)
					continue;

				FString TrackName;
				TrackObj->TryGetStringField(TEXT("track_name"), TrackName);

				FTTVectorTrack VectorTrack;
				VectorTrack.SetTrackName(FName(*TrackName), TimelineTemplate);

				FName CurveName = *FString::Printf(TEXT("%s_%s_Curve"),
					*TimelineName, *TrackName);
				UCurveVector* CurveVector = NewObject<UCurveVector>(
					Blueprint->GeneratedClass, CurveName);
				VectorTrack.CurveVector = CurveVector;

				FString Interp = TEXT("linear");
				TrackObj->TryGetStringField(TEXT("interpolation"), Interp);

				const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
				if (TrackObj->TryGetArrayField(TEXT("keys"), KeysArray))
				{
					for (const auto& KeyVal : *KeysArray)
					{
						const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
						if (!KeyObj)
							continue;

						double Time = 0.0, X = 0.0, Y = 0.0, Z = 0.0;
						KeyObj->TryGetNumberField(TEXT("time"), Time);
						KeyObj->TryGetNumberField(TEXT("x"), X);
						KeyObj->TryGetNumberField(TEXT("y"), Y);
						KeyObj->TryGetNumberField(TEXT("z"), Z);

						ERichCurveInterpMode InterpMode = ERichCurveInterpMode::RCIM_Linear;
						if (Interp == TEXT("constant"))
							InterpMode = ERichCurveInterpMode::RCIM_Constant;
						else if (Interp == TEXT("cubic"))
							InterpMode = ERichCurveInterpMode::RCIM_Cubic;

						for (int32 Axis = 0; Axis < 3; ++Axis)
						{
							double Val = (Axis == 0) ? X : (Axis == 1) ? Y
																	   : Z;
							FKeyHandle Handle = CurveVector->FloatCurves[Axis].AddKey(
								static_cast<float>(Time), static_cast<float>(Val));
							CurveVector->FloatCurves[Axis].SetKeyInterpMode(Handle, InterpMode);
						}

						MaxKeyTime = FMath::Max(MaxKeyTime, Time);
					}
				}

				TimelineTemplate->VectorTracks.Add(VectorTrack);
				TimelineTemplate->AddDisplayTrack(
					FTTTrackId(FTTTrackBase::TT_VectorInterp,
						TimelineTemplate->VectorTracks.Num() - 1));
			}
		}

		// --- Add event tracks ---
		const TArray<TSharedPtr<FJsonValue>>* EventTracksArray = nullptr;
		if (Params->TryGetArrayField(TEXT("event_tracks"), EventTracksArray))
		{
			for (const auto& TrackVal : *EventTracksArray)
			{
				const TSharedPtr<FJsonObject>& TrackObj = TrackVal->AsObject();
				if (!TrackObj)
					continue;

				FString TrackName;
				TrackObj->TryGetStringField(TEXT("track_name"), TrackName);

				FTTEventTrack EventTrack;
				EventTrack.SetTrackName(FName(*TrackName), TimelineTemplate);

				FName CurveName = *FString::Printf(TEXT("%s_%s_EventCurve"),
					*TimelineName, *TrackName);
				UCurveFloat* EventCurve = NewObject<UCurveFloat>(
					Blueprint->GeneratedClass, CurveName);
				EventTrack.CurveKeys = EventCurve;

				const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
				if (TrackObj->TryGetArrayField(TEXT("keys"), KeysArray))
				{
					for (const auto& KeyVal : *KeysArray)
					{
						const TSharedPtr<FJsonObject>& KeyObj = KeyVal->AsObject();
						if (!KeyObj)
							continue;

						double Time = 0.0;
						KeyObj->TryGetNumberField(TEXT("time"), Time);

						// Event tracks use value 1.0 at each trigger time
						EventCurve->FloatCurve.AddKey(
							static_cast<float>(Time), 1.0f);

						MaxKeyTime = FMath::Max(MaxKeyTime, Time);
					}
				}

				TimelineTemplate->EventTracks.Add(EventTrack);
				TimelineTemplate->AddDisplayTrack(
					FTTTrackId(FTTTrackBase::TT_Event,
						TimelineTemplate->EventTracks.Num() - 1));
			}
		}

		// Set timeline length
		double ExplicitLength = 0.0;
		if (Params->TryGetNumberField(TEXT("length"), ExplicitLength))
		{
			TimelineTemplate->TimelineLength = static_cast<float>(ExplicitLength);
		}
		else
		{
			// Auto-derive from latest keyframe
			TimelineTemplate->TimelineLength = static_cast<float>(MaxKeyTime);
		}

		NewNode = TimelineNode;
		NodeDescription = FString::Printf(TEXT("Timeline: %s"), *TimelineName);
	}
	// --- Delegate binding node types ---
	else if (NodeType == TEXT("AddDelegate"))
	{
		FString DelegateName, TargetClass;
		if (!Params->TryGetStringField(TEXT("delegate_name"), DelegateName))
		{
			return MakeErrorResult(TEXT("Missing required field 'delegate_name' for AddDelegate node"));
		}
		Params->TryGetStringField(TEXT("target_class"), TargetClass);

		UK2Node_AddDelegate* DelegateNode = NewObject<UK2Node_AddDelegate>(Graph);
		FString Error = ResolveAndSetDelegate(DelegateNode, DelegateName, TargetClass);
		if (!Error.IsEmpty())
		{
			return MakeErrorResult(Error);
		}

		NewNode = DelegateNode;
		NodeDescription = FString::Printf(TEXT("Bind %s"), *DelegateName);
	}
	else if (NodeType == TEXT("RemoveDelegate"))
	{
		FString DelegateName, TargetClass;
		if (!Params->TryGetStringField(TEXT("delegate_name"), DelegateName))
		{
			return MakeErrorResult(TEXT("Missing required field 'delegate_name' for RemoveDelegate node"));
		}
		Params->TryGetStringField(TEXT("target_class"), TargetClass);

		UK2Node_RemoveDelegate* DelegateNode = NewObject<UK2Node_RemoveDelegate>(Graph);
		FString Error = ResolveAndSetDelegate(DelegateNode, DelegateName, TargetClass);
		if (!Error.IsEmpty())
		{
			return MakeErrorResult(Error);
		}

		NewNode = DelegateNode;
		NodeDescription = FString::Printf(TEXT("Unbind %s"), *DelegateName);
	}
	else if (NodeType == TEXT("ClearDelegate"))
	{
		FString DelegateName, TargetClass;
		if (!Params->TryGetStringField(TEXT("delegate_name"), DelegateName))
		{
			return MakeErrorResult(TEXT("Missing required field 'delegate_name' for ClearDelegate node"));
		}
		Params->TryGetStringField(TEXT("target_class"), TargetClass);

		UK2Node_ClearDelegate* DelegateNode = NewObject<UK2Node_ClearDelegate>(Graph);
		FString Error = ResolveAndSetDelegate(DelegateNode, DelegateName, TargetClass);
		if (!Error.IsEmpty())
		{
			return MakeErrorResult(Error);
		}

		NewNode = DelegateNode;
		NodeDescription = FString::Printf(TEXT("Clear %s"), *DelegateName);
	}
	else if (NodeType == TEXT("CallDelegate"))
	{
		FString DelegateName, TargetClass;
		if (!Params->TryGetStringField(TEXT("delegate_name"), DelegateName))
		{
			return MakeErrorResult(TEXT("Missing required field 'delegate_name' for CallDelegate node"));
		}
		Params->TryGetStringField(TEXT("target_class"), TargetClass);

		UK2Node_CallDelegate* DelegateNode = NewObject<UK2Node_CallDelegate>(Graph);
		FString Error = ResolveAndSetDelegate(DelegateNode, DelegateName, TargetClass);
		if (!Error.IsEmpty())
		{
			return MakeErrorResult(Error);
		}

		NewNode = DelegateNode;
		NodeDescription = FString::Printf(TEXT("Call %s"), *DelegateName);
	}
	else if (NodeType == TEXT("CreateDelegate"))
	{
		FString FunctionName;
		if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
		{
			return MakeErrorResult(TEXT("Missing required field 'function_name' for CreateDelegate node"));
		}

		UK2Node_CreateDelegate* CreateDelegateNode = NewObject<UK2Node_CreateDelegate>(Graph);
		CreateDelegateNode->SelectedFunctionName = FName(*FunctionName);

		NewNode = CreateDelegateNode;
		NodeDescription = FString::Printf(TEXT("Create Delegate: %s"), *FunctionName);
	}
	else if (NodeType == TEXT("AssignDelegate"))
	{
		FString DelegateName, TargetClass;
		if (!Params->TryGetStringField(TEXT("delegate_name"), DelegateName))
		{
			return MakeErrorResult(TEXT("Missing required field 'delegate_name' for AssignDelegate node"));
		}
		Params->TryGetStringField(TEXT("target_class"), TargetClass);

		UK2Node_AssignDelegate* AssignNode = NewObject<UK2Node_AssignDelegate>(Graph);
		FString Error = ResolveAndSetDelegate(AssignNode, DelegateName, TargetClass);
		if (!Error.IsEmpty())
		{
			return MakeErrorResult(Error);
		}

		// AssignDelegate handles its own graph insertion because it needs to create
		// a companion CustomEvent node after pins are allocated
		AssignNode->NodePosX = Position.X;
		AssignNode->NodePosY = Position.Y;
		AssignNode->CreateNewGuid();
		Graph->AddNode(AssignNode, false, false);
		AssignNode->AllocateDefaultPins();

		// Create companion CustomEvent with matching delegate signature
		UFunction* DelegateSignature = AssignNode->GetDelegateSignature();
		FString EventName;
		if (!Params->TryGetStringField(TEXT("event_name"), EventName))
		{
			EventName = FString::Printf(TEXT("%s_Event"), *DelegateName);
		}

		if (DelegateSignature)
		{
			UK2Node_CustomEvent* EventNode = UK2Node_CustomEvent::CreateFromFunction(
				FVector2D(Position.X - 150, Position.Y + 150),
				Graph, EventName, DelegateSignature, /*bSelectNewNode=*/false);

			if (EventNode)
			{
				// Wire the custom event's delegate output to the AssignDelegate's delegate input
				UEdGraphPin* DelegatePin = AssignNode->GetDelegatePin();
				UEdGraphPin* EventDelegatePin = EventNode->FindPin(UK2Node_Event::DelegateOutputName);
				if (DelegatePin && EventDelegatePin)
				{
					const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
					K2Schema->TryCreateConnection(EventDelegatePin, DelegatePin);
				}

				// Include companion event in affected nodes set
				Data->LastOperationAffectedNodes.Add(EventNode->NodeGuid);
			}
		}

		bNodeAlreadyAdded = true;
		NewNode = AssignNode;
		NodeDescription = FString::Printf(TEXT("Assign %s"), *DelegateName);
	}
	else if (NodeType == TEXT("ComponentBoundEvent"))
	{
		FString ComponentName, DelegateName;
		if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
		{
			return MakeErrorResult(TEXT("Missing required field 'component_name' for ComponentBoundEvent node"));
		}
		if (!Params->TryGetStringField(TEXT("delegate_name"), DelegateName))
		{
			return MakeErrorResult(TEXT("Missing required field 'delegate_name' for ComponentBoundEvent node"));
		}

		// 1. Resolve the FObjectProperty for the component on the Blueprint class.
		//    Use SkeletonGeneratedClass first (matches ResolveAndSetDelegate idiom),
		//    fall back to GeneratedClass. This covers both SCS-declared components on
		//    this Blueprint and C++/inherited components on any parent class -- all
		//    surface as FObjectProperty on the skeleton class once the BP is compiled.
		UClass* BPClass = Blueprint->SkeletonGeneratedClass
			? Blueprint->SkeletonGeneratedClass
			: Blueprint->GeneratedClass;
		if (!BPClass)
		{
			return MakeErrorResult(TEXT("ComponentBoundEvent: Blueprint has no generated class yet (compile the BP first)"));
		}
		FObjectProperty* ComponentProp = FindFProperty<FObjectProperty>(BPClass, FName(*ComponentName));
		if (!ComponentProp || !ComponentProp->PropertyClass
			|| !ComponentProp->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("ComponentBoundEvent: component '%s' not found on Blueprint class '%s' (or is not a UActorComponent-derived property)"),
				*ComponentName, *GetNameSafe(BPClass)));
		}
		UClass* ComponentClass = ComponentProp->PropertyClass;

		// 2. Validate delegate exists on component's class as a multicast delegate.
		FMulticastDelegateProperty* DelegateProp = FindFProperty<FMulticastDelegateProperty>(
			ComponentClass, FName(*DelegateName));
		if (!DelegateProp)
		{
			FProperty* Prop = ComponentClass->FindPropertyByName(FName(*DelegateName));
			if (Prop)
			{
				return MakeErrorResult(FString::Printf(
					TEXT("Property '%s' on component class '%s' is not a multicast delegate (actual: %s)"),
					*DelegateName, *GetNameSafe(ComponentClass), *Prop->GetClass()->GetName()));
			}
			return MakeErrorResult(FString::Printf(
				TEXT("Multicast delegate '%s' not found on component class '%s'"),
				*DelegateName, *GetNameSafe(ComponentClass)));
		}

		// 3. Reject duplicate bindings -- engine's CanPasteHere uses the same check
		//    (K2Node_ComponentBoundEvent.cpp:73). Creating a second one produces a
		//    compile warning: "There can only be one event node bound to this component!".
		if (FKismetEditorUtilities::FindBoundEventForComponent(Blueprint, FName(*DelegateName), FName(*ComponentName)))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("ComponentBoundEvent: %s.%s is already bound in this Blueprint (duplicate not allowed)"),
				*ComponentName, *DelegateName));
		}

		// 4. Create, add to graph, allocate pins, initialize params, reconstruct.
		//    Follows the canonical spawner sequence (BlueprintBoundEventNodeSpawner.cpp:180-183):
		//    InitializeComponentBoundEventParams -> ReconstructNode.
		UK2Node_ComponentBoundEvent* BoundEvent = NewObject<UK2Node_ComponentBoundEvent>(Graph);
		BoundEvent->NodePosX = Position.X;
		BoundEvent->NodePosY = Position.Y;
		BoundEvent->CreateNewGuid();
		Graph->AddNode(BoundEvent, false, false);
		BoundEvent->AllocateDefaultPins();
		BoundEvent->InitializeComponentBoundEventParams(ComponentProp, DelegateProp);
		BoundEvent->ReconstructNode();

		bNodeAlreadyAdded = true;
		NewNode = BoundEvent;
		NodeDescription = FString::Printf(TEXT("Bound Event: %s.%s"), *ComponentName, *DelegateName);
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Unsupported node type: %s. Use 'Generic' with 'class_name' parameter for custom node types."), *NodeType));
	}

	if (!NewNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create node of type: %s"), *NodeType));
	}

	if (!bNodeAlreadyAdded)
	{
		// If auto-connect is enabled and we have a cursor node, calculate position relative to it
		if (bAutoConnect && Data->Cursor.FocusedNodeGuid.IsValid())
		{
			UEdGraphNode* CursorNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, Data->Cursor.FocusedNodeGuid);
			if (CursorNode && !Params->HasField(TEXT("position")))
			{
				// Place to the right of cursor node
				Position.X = CursorNode->NodePosX + 300.0f;
				Position.Y = CursorNode->NodePosY;
			}
		}

		// Set position and add to graph
		NewNode->NodePosX = Position.X;
		NewNode->NodePosY = Position.Y;
		NewNode->CreateNewGuid();
		Graph->AddNode(NewNode, false, false);

		// Allocate default pins
		NewNode->AllocateDefaultPins();
	}

	// Handle num_extra_pins for dynamic-pin nodes
	{
		int32 NumExtraPins = 0;
		if (Params->TryGetNumberField(TEXT("num_extra_pins"), NumExtraPins) && NumExtraPins > 0)
		{
			NumExtraPins = FMath::Clamp(NumExtraPins, 0, 50);

			IK2Node_AddPinInterface* AddPinIface = Cast<IK2Node_AddPinInterface>(NewNode);
			UK2Node_Switch* SwitchNode = Cast<UK2Node_Switch>(NewNode);

			if (AddPinIface)
			{
				for (int32 i = 0; i < NumExtraPins && AddPinIface->CanAddPin(); ++i)
				{
					AddPinIface->AddInputPin();
				}
			}
			else if (SwitchNode && !SwitchNode->IsA<UK2Node_SwitchEnum>())
			{
				for (int32 i = 0; i < NumExtraPins; ++i)
				{
					SwitchNode->AddPinToSwitchNode();
				}
			}
		}
	}

	// Auto-connect if requested
	FString AutoConnectMessage;
	if (bAutoConnect && Data->Cursor.FocusedNodeGuid.IsValid() && Data->Cursor.FocusedPinName != NAME_None)
	{
		UEdGraphNode* CursorNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, Data->Cursor.FocusedNodeGuid);
		if (CursorNode)
		{
			UEdGraphPin* CursorPin = CursorNode->FindPin(Data->Cursor.FocusedPinName, Data->Cursor.FocusedPinDirection);
			if (CursorPin)
			{
				// Find compatible pin on new node
				TArray<UEdGraphPin*> CompatiblePins = ClaireonBlueprintHelpers::FindCompatiblePins(NewNode, CursorPin);
				if (CompatiblePins.Num() > 0)
				{
					UEdGraphPin* TargetPin = CompatiblePins[0];
					const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

					if (CursorPin->Direction == EGPD_Output)
					{
						K2Schema->TryCreateConnection(CursorPin, TargetPin);
						AutoConnectMessage = FString::Printf(TEXT("\nAuto-connected: %s -> %s"), *CursorPin->PinName.ToString(), *TargetPin->PinName.ToString());
					}
					else
					{
						K2Schema->TryCreateConnection(TargetPin, CursorPin);
						AutoConnectMessage = FString::Printf(TEXT("\nAuto-connected: %s -> %s"), *TargetPin->PinName.ToString(), *CursorPin->PinName.ToString());
					}
				}
			}
		}
	}

	// Mark Blueprint as structurally modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// Move cursor to new node
	Data->Cursor.PushHistory(Data->Cursor.GraphName);
	Data->Cursor.FocusedNodeGuid = NewNode->NodeGuid;
	UEdGraphPin* FirstOutputPin = ClaireonBlueprintHelpers::GetFirstOutputPin(NewNode);
	if (FirstOutputPin)
	{
		Data->Cursor.FocusedPinName = FirstOutputPin->PinName;
		Data->Cursor.FocusedPinDirection = FirstOutputPin->Direction;
	}

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Added node: %s at (%.0f, %.0f)%s"),
		*NodeDescription, Position.X, Position.Y, *AutoConnectMessage);

	// Populate affected nodes: new node + exec-connected neighbors
	Data->LastOperationAffectedNodes.Add(NewNode->NodeGuid);
	for (UEdGraphPin* AffPin : NewNode->Pins)
	{
		if (AffPin && AffPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			for (UEdGraphPin* LinkedAffPin : AffPin->LinkedTo)
			{
				if (LinkedAffPin && LinkedAffPin->GetOwningNode())
				{
					Data->LastOperationAffectedNodes.Add(LinkedAffPin->GetOwningNode()->NodeGuid);
				}
			}
		}
	}

	FToolResult AddNodeResult = BuildStateResponse(SessionId, Data);
	AddNodeResult.Warnings.Append(ResolutionWarnings);
	return AddNodeResult;
}

// ----------------------------------------------------------------------------
// P1: hot-path metadata enrichment
// ----------------------------------------------------------------------------

FString ClaireonBlueprintGraphTool_AddNode::GetFullDescription() const
{
    return TEXT(
        "Adds a node to the current session's graph. Supports CallFunction, "
        "VariableGet/VariableSet, control flow (Branch/Sequence/ForEach), "
        "macros, delegates, custom events, casts, and timeline nodes. The "
        "preferred wiring path is auto_connect_from_cursor=true: when the "
        "session cursor sits on a pin compatible with the new node's exec "
        "input, the connection is made automatically without requiring a "
        "follow-up bp_connect_pins call.");
}

FString ClaireonBlueprintGraphTool_AddNode::GetPatterns() const
{
    // Part C (#0000): save-discipline guidance migrates here from
    // GetFullDescription so tool_search deep-inspect can surface it under a
    // dedicated `patterns` field. ASCII only; no em-dashes.
    return TEXT(
        "## Common pitfalls\n"
        "\n"
        "Per the per-node cycle in the per-tool authoring guidance, save "
        "every 1-3 add_node calls via bp_save to flush in-session edits to "
        "the asset and protect against editor-crash data loss.\n"
        "\n"
        "## See also\n"
        "\n"
        "- claireon.bp_connect_pins -- companion when auto_connect_from_cursor "
        "doesn't fit\n"
        "- claireon.bp_save -- per-node-cycle save discipline\n");
}

FString ClaireonBlueprintGraphTool_AddNode::GetExampleUsage() const
{
    return TEXT(
        "bp_add_node session_id=\"...\" "
        "node_class=\"K2Node_CallFunction\" function=\"PrintString\" "
        "auto_connect_from_cursor=true");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddNode::GetParameterTooltips() const
{
    TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
    T->SetStringField(TEXT("session_id"), TEXT("Session ID returned by bp_open or _create. Optional if asset_path is supplied."));
    T->SetStringField(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id; auto-opens the session)."));
    T->SetStringField(TEXT("node_class"), TEXT("Fully qualified UClass name of the K2Node to add (e.g. K2Node_CallFunction, K2Node_IfThenElse). Fuzzy-resolved (drop U prefix; partial matches allowed)."));
    T->SetStringField(TEXT("function"), TEXT("For K2Node_CallFunction: the function name (or Class.Function) to call. Resolved against UFUNCTION metadata."));
    T->SetStringField(TEXT("auto_connect_from_cursor"), TEXT("If true, route the new node's exec pin through the cursor pin when compatible. Preferred over a follow-up bp_connect_pins call."));
    return T;
}

#undef LOCTEXT_NAMESPACE
