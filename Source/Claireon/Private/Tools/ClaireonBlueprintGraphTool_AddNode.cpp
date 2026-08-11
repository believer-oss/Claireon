// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonBlueprintNodeTypeRegistry.h"
#include "ClaireonBlueprintNodeFactory.h"
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
#include "K2Node_AddComponent.h"
#include "GameFramework/Actor.h"
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
    return TEXT("Add a node to the current session's graph (CallFunction, VariableGet/Set, control flow, macros, delegates, etc.). Pass auto_connect_from_cursor=true to route the new node's exec pin through the cursor pin when compatible. Save every 1-3 nodes via bp_save to avoid losing work on editor crash. Accepts session_id or asset_path; auto-opens when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddNode::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("node_type"), TEXT("Node class alias or full name (e.g. CallFunction, AsyncAction, VariableGet, Branch, Sequence, Cast, MacroInstance, MultiGate, FormatText, SwitchGameplayTag, Composite, Timeline, ComponentBoundEvent, AddComponent, EventOverride, or a K2Node_* class name; unknown K2Node classes route through the Generic path)."), true);
    Builder.AddString(TEXT("function_name"), TEXT("Function name. REQUIRED for CallFunction/AsyncAction/CallParentFunction/EventOverride/CreateDelegate; used with function_class on Generic async-task subclasses to initialize proxy fields."));
    Builder.AddString(TEXT("function_class"), TEXT("Class that owns the function (for CallFunction/AsyncAction/Generic async nodes)."));
    Builder.AddString(TEXT("node_class"), TEXT("For CallFunction: exact UK2Node_CallFunction subclass to instantiate (e.g. K2Node_PromotableOperator, K2Node_CallArrayFunction). Overrides the class inferred from the function's metadata; replay callers pass the source node's class."));
    Builder.AddString(TEXT("variable_name"), TEXT("Variable name for VariableGet/Set."));
    Builder.AddString(TEXT("member_parent"), TEXT("For VariableGet/Set: external class owning the member (e.g. 'SceneComponent' for RelativeLocation). Omit for self-context members. target_class is accepted as an alias."));
    Builder.AddString(TEXT("member_scope"), TEXT("For VariableGet/Set: function graph name declaring a function-local variable (e.g. 'Get Warp Target Transform'). Use bp_add_local_variable to declare locals."));
    Builder.AddBoolean(TEXT("validated"), TEXT("For VariableGet: create a validated (impure) get with execute/then/else exec pins instead of a pure value tap."));
    Builder.AddString(TEXT("macro_library"), TEXT("Macro library asset path for Macro/MacroInstance (default StandardMacros; macro_library_path also accepted)."));
    Builder.AddString(TEXT("macro_name"), TEXT("Macro graph name for Macro/MacroInstance (may contain spaces, e.g. 'Switch Has Authority')."));
    Builder.AddString(TEXT("target_class"), TEXT("REQUIRED for Cast: class to cast to. Also accepted as an actor_class alias for SpawnActor, as a member_parent alias for VariableGet/Set, and as the delegate owner class for delegate nodes."));
    Builder.AddString(TEXT("actor_class"), TEXT("REQUIRED for SpawnActor: the AActor subclass to spawn (target_class is accepted as an alias; actor_class wins when both are present)."));
    Builder.AddString(TEXT("struct_type"), TEXT("Struct type path for MakeStruct/BreakStruct."));
    Builder.AddString(TEXT("enum_type"), TEXT("Enum type path for SwitchEnum/ForEachElementInEnum."));
    Builder.AddString(TEXT("event_name"), TEXT("Event name. REQUIRED for CustomEvent; optional companion-event name for AssignDelegate."));
    Builder.AddString(TEXT("comment_text"), TEXT("Comment text for Comment nodes."));
    Builder.AddString(TEXT("delegate_name"), TEXT("REQUIRED for AddDelegate/RemoveDelegate/ClearDelegate/CallDelegate/AssignDelegate/ComponentBoundEvent: the multicast delegate property name."));
    Builder.AddString(TEXT("component_name"), TEXT("REQUIRED for ComponentBoundEvent: the component property the delegate lives on."));
    Builder.AddString(TEXT("component_class"), TEXT("REQUIRED for AddComponent: the UActorComponent subclass whose template the node spawns. A template is provisioned in the Blueprint's ComponentTemplates array and the ReturnValue pin types from it."));
    Builder.AddString(TEXT("template_name"), TEXT("For AddComponent: desired component-template object name (used when free; replay passes the source's name so the TemplateName pin default matches). Defaults to an engine-generated unique name."));
    Builder.AddString(TEXT("timeline_name"), TEXT("REQUIRED for Timeline nodes. Timeline also accepts autoplay/loop booleans, length number, and float_tracks/vector_tracks/event_tracks arrays ([{track_name, interpolation, keys:[{time,value|x,y,z}]}]) to author track curves at create time."));
    Builder.AddString(TEXT("class_name"), TEXT("For Generic: the UEdGraphNode subclass to instantiate (e.g. a project K2Node class)."));
    Builder.AddObject(TEXT("node_properties"), TEXT("For Generic: reflection property bag applied before pin allocation (e.g. ProxyFactoryClass/ProxyFactoryFunctionName/ProxyClass for async-task nodes). Supports bool/int/float/string/name, class/enum/struct-type and asset-path object references, struct text values (e.g. FKey), and enum entries."));
    Builder.AddString(TEXT("input_action"), TEXT("For EnhancedInputAction (Generic path): object path of the UInputAction asset that gives the node its identity."));
    Builder.AddString(TEXT("key"), TEXT("For InputDebugKey/InputKey (Generic path): FKey name (e.g. 'F', 'Gamepad_FaceButton_Bottom')."));
    Builder.AddString(TEXT("graph_name"), TEXT("For Composite/CollapsedGraph: name for the new collapsed subgraph."));
    Builder.AddString(TEXT("format_text"), TEXT("For FormatText: the format string; {Argument} markers synthesize the argument pins."));
    Builder.AddArray(TEXT("tags"), TEXT("For SwitchGameplayTag: array of gameplay tag names, one case pin per tag."));
    Builder.AddArray(TEXT("user_defined_pins"), TEXT("For CustomEvent: array of {name, type} parameter pins (type uses the variable_type string grammar)."));
    Builder.AddNumber(TEXT("num_extra_pins"), TEXT("Extra dynamic pins for Sequence/MakeArray/Switch-style nodes."));
    Builder.AddBoolean(TEXT("auto_connect_from_cursor"), TEXT("If true, auto-connect the new node to the session cursor pin when a compatible pin exists. The status reports whether the connection was actually made; a failed connection does not fail the node add."));
    Builder.AddNumber(TEXT("position_x"), TEXT("Optional X coordinate; defaults to cursor. A position={x,y} object is also accepted and wins for any axis it carries when both forms are present."));
    Builder.AddNumber(TEXT("position_y"), TEXT("Optional Y coordinate; defaults to cursor. See position_x for precedence with the position object."));
    Builder.AddObject(TEXT("position"), TEXT("Optional {x,y} position object; alternative to position_x/position_y. When both forms are present the object wins for any axis it carries."));
    // Read at Execute for the FunctionResult branch and never declared until now.
    Builder.AddBoolean(TEXT("force_new"), TEXT("FunctionResult only: author an ADDITIONAL Return Node instead of returning the existing one. Functions may hold one per exec branch."));
    Builder.AddBoolean(TEXT("autoplay"), TEXT("Timeline only: sets bAutoPlay on the new timeline template (default false)."));
    Builder.AddBoolean(TEXT("loop"), TEXT("Timeline only: sets bLoop on the new timeline template (default false)."));
    Builder.AddNumber(TEXT("length"), TEXT("Timeline only: explicit timeline length in seconds; defaults to the latest authored keyframe time."));
    Builder.AddArray(TEXT("float_tracks"), TEXT("Timeline only: [{track_name, interpolation('linear'|'cubic'|'constant'), keys:[{time,value}]}] float tracks authored at create time."));
    Builder.AddArray(TEXT("vector_tracks"), TEXT("Timeline only: [{track_name, interpolation('linear'|'cubic'|'constant'), keys:[{time,x,y,z}]}] vector tracks authored at create time."));
    Builder.AddArray(TEXT("event_tracks"), TEXT("Timeline only: [{track_name, keys:[{time}]}] event trigger tracks authored at create time."));
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

	if (!IsValid(Blueprint) || !IsValid(Graph))
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

	// SpawnActor spawn-class alias: the factory reads 'actor_class', but an
	// earlier schema described target_class as the spawn field, so accept both.
	// Rewrite here (this tool owns the contract; the factory stays untouched).
	// actor_class wins when both are supplied.
	if (NodeType == TEXT("SpawnActor") && !Params->HasField(TEXT("actor_class")))
	{
		FString SpawnClassAlias;
		if (Params->TryGetStringField(TEXT("target_class"), SpawnClassAlias))
		{
			Params->SetStringField(TEXT("actor_class"), SpawnClassAlias);
		}
	}

	// Get optional position. The schema advertises position_x/position_y number
	// fields and a position={x,y} object. Explicit coordinates in either form
	// take precedence over the cursor default; when both forms are present the
	// object wins for any axis it carries. An axis absent from every form keeps
	// the cursor default -- a partial object is never silently zero-defaulted.
	FVector2D Position = Data->Cursor.ViewportCenter;
	bool bExplicitPosition = false;
	{
		double ScalarX = 0.0, ScalarY = 0.0;
		const bool bHasScalarX = Params->TryGetNumberField(TEXT("position_x"), ScalarX);
		const bool bHasScalarY = Params->TryGetNumberField(TEXT("position_y"), ScalarY);
		if (bHasScalarX) { Position.X = ScalarX; bExplicitPosition = true; }
		if (bHasScalarY) { Position.Y = ScalarY; bExplicitPosition = true; }
	}
	const TSharedPtr<FJsonObject>* PositionObj = nullptr;
	if (Params->TryGetObjectField(TEXT("position"), PositionObj))
	{
		double ObjX = 0.0, ObjY = 0.0;
		if ((*PositionObj)->TryGetNumberField(TEXT("x"), ObjX)) { Position.X = ObjX; bExplicitPosition = true; }
		if ((*PositionObj)->TryGetNumberField(TEXT("y"), ObjY)) { Position.Y = ObjY; bExplicitPosition = true; }
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
			OwnerClass = IsValid(Blueprint->SkeletonGeneratedClass)
				? Blueprint->SkeletonGeneratedClass
				: Blueprint->ParentClass;
		}
		else
		{
			ClaireonNameResolver::FNameResolveResult DelegateClassResult;
			OwnerClass = ClaireonNameResolver::ResolveClassName(TargetClass, nullptr, DelegateClassResult);
			if (!IsValid(OwnerClass))
			{
				return DelegateClassResult.Error;
			}
			if (!DelegateClassResult.ResolutionNote.IsEmpty())
			{
				ResolutionWarnings.Add(DelegateClassResult.ResolutionNote);
			}
		}

		if (!IsValid(OwnerClass))
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

	// Factory dispatch: delegate node construction to ClaireonBlueprintNodeFactory::CreateNode
	// for all node_types it supports. The factory is the single source of truth for these
	// branches; the inline cases that remain below cover types the factory does not yet
	// handle (EventOverride, FunctionEntry, FunctionResult, Tunnel, Timeline, AddDelegate,
	// RemoveDelegate, ClearDelegate, CallDelegate, CreateDelegate, AssignDelegate,
	// ComponentBoundEvent).
	// Membership comes from the shared node-type registry rather than a set literal
	// here, so bp_list_node_types describes exactly what this dispatch accepts.
	if (ClaireonBlueprintNodeTypes::IsFactoryHandled(NodeType))
	{
		ClaireonBlueprintNodeFactory::FCreateResult R =
			ClaireonBlueprintNodeFactory::CreateNode(Blueprint, Graph, Params, Position);
		if (!R.IsOk())
		{
			return MakeErrorResult(R.Error);
		}
		NewNode = R.Node;
		NodeDescription = R.Description;
		// Factory contract: bAlreadyAdded is true for typed branches (Graph->AddNode +
		// AllocateDefaultPins + ReconstructNode-where-needed have already run).
		bNodeAlreadyAdded = R.bAlreadyAdded;
		ResolutionWarnings.Append(R.Warnings);
	}
	// Inline branches below cover node types not yet absorbed by the factory.
	else if (NodeType == TEXT("EventOverride"))
	{
		FString FunctionName;
		if (!Params->TryGetStringField(TEXT("function_name"), FunctionName))
		{
			return MakeErrorResult(TEXT("Missing required field 'function_name' for EventOverride node"));
		}

		UClass* ParentClass = Blueprint->ParentClass;
		ClaireonNameResolver::FNameResolveResult EventFuncResult;
		UFunction* TargetFunc = IsValid(ParentClass)
			? ClaireonNameResolver::ResolveFunctionName(ParentClass, FunctionName, EventFuncResult)
			: nullptr;
		UClass* EventSourceClass = ParentClass;

		// Interface events: the overridden function may live on an implemented
		// interface class rather than in the parent chain (BP interface function
		// FNames can even carry spaces, e.g. 'Empower Ability'). Search each
		// implemented interface on a parent-chain miss.
		if (!IsValid(TargetFunc))
		{
			for (const FBPInterfaceDescription& IfaceDesc : Blueprint->ImplementedInterfaces)
			{
				UClass* IfaceClass = IfaceDesc.Interface.Get();
				if (!IsValid(IfaceClass))
				{
					continue;
				}
				ClaireonNameResolver::FNameResolveResult IfaceResult;
				if (UFunction* IfaceFunc = ClaireonNameResolver::ResolveFunctionName(IfaceClass, FunctionName, IfaceResult); IsValid(IfaceFunc))
				{
					TargetFunc = IfaceFunc;
					EventSourceClass = IfaceClass;
					if (!IfaceResult.ResolutionNote.IsEmpty())
					{
						ResolutionWarnings.Add(IfaceResult.ResolutionNote);
					}
					break;
				}
			}
		}

		if (!IsValid(TargetFunc))
		{
			return MakeErrorResult(EventFuncResult.Error.IsEmpty()
					? FString::Printf(TEXT("Function '%s' not found: Blueprint has no parent class"), *FunctionName)
					: FString::Printf(TEXT("%s (also searched %d implemented interface(s))"),
						*EventFuncResult.Error, Blueprint->ImplementedInterfaces.Num()));
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
			Blueprint, EventSourceClass, TargetFunc->GetFName());
		if (IsValid(ExistingOverride))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Override for '%s' already exists (node GUID: %s)"),
				*TargetFunc->GetName(), *ExistingOverride->NodeGuid.ToString()));
		}

		UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
		EventNode->EventReference.SetExternalMember(TargetFunc->GetFName(), EventSourceClass);
		EventNode->bOverrideFunction = true;

		NewNode = EventNode;
		NodeDescription = FString::Printf(TEXT("Event Override: %s"), *TargetFunc->GetName());
	}
	else if (NodeType == TEXT("FunctionEntry"))
	{
		// Find-or-return on the function's entry node (function graphs always have exactly one).
		// Creation is never attempted; the entry node is seeded when the function graph is created.
		const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
		const EGraphType GraphType = IsValid(K2Schema) ? K2Schema->GetGraphType(Graph) : GT_MAX;
		if (GraphType != GT_Function)
		{
			return MakeErrorResult(TEXT("FunctionEntry can only be added to a function graph"));
		}

		UK2Node_FunctionEntry* EntryNode = nullptr;
		for (UEdGraphNode* ExistingNode : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(ExistingNode); IsValid(AsEntry))
			{
				EntryNode = AsEntry;
				break;
			}
		}

		if (!IsValid(EntryNode))
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
		const EGraphType GraphType = IsValid(K2Schema) ? K2Schema->GetGraphType(Graph) : GT_MAX;
		if (GraphType != GT_Function)
		{
			return MakeErrorResult(TEXT("FunctionResult can only be added to a function graph"));
		}

		UK2Node_FunctionEntry* EntryNode = nullptr;
		for (UEdGraphNode* ExistingNode : Graph->Nodes)
		{
			if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(ExistingNode); IsValid(AsEntry))
			{
				EntryNode = AsEntry;
				break;
			}
		}
		if (!IsValid(EntryNode))
		{
			return MakeErrorResult(TEXT("Function graph has no entry node; cannot add return node"));
		}

		// force_new: author an ADDITIONAL Return Node (functions may hold one per
		// exec branch). PostPlacedNewNode syncs the new node's pins with the entry
		// signature and any pre-existing result node. Default (find-or-create)
		// returns the existing node.
		bool bForceNew = false;
		Params->TryGetBoolField(TEXT("force_new"), bForceNew);

		UK2Node_FunctionResult* ResultNode = nullptr;
		if (bForceNew)
		{
			ResultNode = NewObject<UK2Node_FunctionResult>(Graph);
			ResultNode->FunctionReference = EntryNode->FunctionReference;
			ResultNode->CreateNewGuid();
			double PosX = 0.0, PosY = 0.0;
			if (Params->TryGetNumberField(TEXT("position_x"), PosX))
			{
				ResultNode->NodePosX = FMath::RoundToInt(PosX);
			}
			if (Params->TryGetNumberField(TEXT("position_y"), PosY))
			{
				ResultNode->NodePosY = FMath::RoundToInt(PosY);
			}
			Graph->Modify();
			Graph->AddNode(ResultNode, /*bUserAction=*/true, /*bSelectNewNode=*/false);
			ResultNode->PostPlacedNewNode();
			ResultNode->AllocateDefaultPins();
		}
		else
		{
			ResultNode = FBlueprintEditorUtils::FindOrCreateFunctionResultNode(EntryNode);
		}
		if (!IsValid(ResultNode))
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
		const EGraphType GraphType = IsValid(K2Schema) ? K2Schema->GetGraphType(Graph) : GT_MAX;
		if (GraphType != GT_Macro)
		{
			return MakeErrorResult(TEXT("Tunnel can only be added to a macro graph"));
		}

		TArray<UK2Node_Tunnel*> TunnelNodes;
		for (UEdGraphNode* ExistingNode : Graph->Nodes)
		{
			if (UK2Node_Tunnel* AsTunnel = Cast<UK2Node_Tunnel>(ExistingNode); IsValid(AsTunnel))
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
	else if (NodeType == TEXT("Timeline"))
	{
		FString TimelineName;
		if (!Params->TryGetStringField(TEXT("timeline_name"), TimelineName))
		{
			return MakeErrorResult(TEXT("Missing required field 'timeline_name' for Timeline node"));
		}

		// Check for duplicate using canonical lookup (handles _Template naming)
		if (IsValid(Blueprint->FindTimelineTemplateByVariableName(FName(*TimelineName))))
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

		if (!IsValid(TimelineTemplate))
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

		if (IsValid(DelegateSignature))
		{
			UK2Node_CustomEvent* EventNode = UK2Node_CustomEvent::CreateFromFunction(
				FVector2D(Position.X - 150, Position.Y + 150),
				Graph, EventName, DelegateSignature, /*bSelectNewNode=*/false);

			if (IsValid(EventNode))
			{
				// Wire the custom event's delegate output to the AssignDelegate's
				// delegate input. TryCreateConnection returns false when the schema
				// rejects the link; surface that instead of silently reporting an
				// unwired companion event as success -- the node add itself still
				// succeeds either way.
				UEdGraphPin* DelegatePin = AssignNode->GetDelegatePin();
				UEdGraphPin* EventDelegatePin = EventNode->FindPin(UK2Node_Event::DelegateOutputName);
				if (DelegatePin && EventDelegatePin)
				{
					const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
					if (!K2Schema->TryCreateConnection(EventDelegatePin, DelegatePin))
					{
						const FPinConnectionResponse WireResponse = K2Schema->CanCreateConnection(EventDelegatePin, DelegatePin);
						const FString WireReason = WireResponse.Message.IsEmpty()
							? FString(TEXT("schema rejected the connection"))
							: WireResponse.Message.ToString();
						ResolutionWarnings.Add(FString::Printf(
							TEXT("Companion event '%s' was created but NOT wired to the Assign node's delegate pin (%s). Wire it manually via bp_connect_pins."),
							*EventName, *WireReason));
					}
				}
				else
				{
					ResolutionWarnings.Add(FString::Printf(
						TEXT("Companion event '%s' was created but its delegate pins could not be located; it is NOT wired to the Assign node. Wire it manually via bp_connect_pins."),
						*EventName));
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
		UClass* BPClass = IsValid(Blueprint->SkeletonGeneratedClass)
			? Blueprint->SkeletonGeneratedClass
			: Blueprint->GeneratedClass;
		if (!IsValid(BPClass))
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
		if (IsValid(FKismetEditorUtilities::FindBoundEventForComponent(Blueprint, FName(*DelegateName), FName(*ComponentName))))
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
	else if (NodeType == TEXT("AddComponent"))
	{
		// Dynamic 'Add Component' node (UK2Node_AddComponent). Mirrors
		// UBlueprintComponentNodeSpawner::Invoke: bind AActor::AddComponent,
		// provision a component template on the GeneratedClass, point the
		// TemplateName pin at it, and type ReturnValue from the template class.
		FString ComponentClassName;
		if (!Params->TryGetStringField(TEXT("component_class"), ComponentClassName) || ComponentClassName.IsEmpty())
		{
			return MakeErrorResult(TEXT("Missing required field 'component_class' for AddComponent node"));
		}

		ClaireonNameResolver::FNameResolveResult CompClassResult;
		UClass* ComponentClass = ClaireonNameResolver::ResolveClassName(ComponentClassName, UActorComponent::StaticClass(), CompClassResult);
		if (!IsValid(ComponentClass))
		{
			return MakeErrorResult(CompClassResult.Error.IsEmpty()
				? FString::Printf(TEXT("AddComponent: component_class '%s' could not be resolved to a UActorComponent subclass"), *ComponentClassName)
				: CompClassResult.Error);
		}
		if (!CompClassResult.ResolutionNote.IsEmpty())
		{
			ResolutionWarnings.Add(CompClassResult.ResolutionNote);
		}

		if (!IsValid(Blueprint->GeneratedClass))
		{
			return MakeErrorResult(TEXT("AddComponent: Blueprint has no generated class yet (compile the BP first)"));
		}
		if (!FBlueprintEditorUtils::IsActorBased(Blueprint))
		{
			return MakeErrorResult(TEXT("AddComponent: only actor-based Blueprints can host Add Component nodes"));
		}

		UFunction* AddComponentFunc = AActor::StaticClass()->FindFunctionByName(
			UK2Node_AddComponent::GetAddComponentFunctionName());
		if (!IsValid(AddComponentFunc))
		{
			return MakeErrorResult(TEXT("AddComponent: AActor::AddComponent function not found (engine drift?)"));
		}

		UK2Node_AddComponent* AddCompNode = NewObject<UK2Node_AddComponent>(Graph);
		AddCompNode->FunctionReference.SetFromField<UFunction>(AddComponentFunc, /*bSelfContext=*/true);
		AddCompNode->TemplateType = ComponentClass;
		AddCompNode->NodePosX = Position.X;
		AddCompNode->NodePosY = Position.Y;
		AddCompNode->CreateNewGuid();
		AddCompNode->SetFlags(RF_Transactional);
		Graph->AddNode(AddCompNode, false, false);
		AddCompNode->AllocateDefaultPins();

		if (UEdGraphPin* ReturnPin = AddCompNode->GetReturnValuePin())
		{
			ReturnPin->PinType.PinSubCategoryObject = ComponentClass;
		}

		// Template name: honor the caller's requested name when it is free on
		// the GeneratedClass outer (replay wants the source's exact name so the
		// TemplateName pin default matches); otherwise fall back to the engine's
		// unique-name generator.
		FString RequestedTemplateName;
		Params->TryGetStringField(TEXT("template_name"), RequestedTemplateName);
		FName TemplateObjectName = NAME_None;
		if (!RequestedTemplateName.IsEmpty()
			&& !IsValid(StaticFindObject(nullptr, Blueprint->GeneratedClass, *RequestedTemplateName, /*ExactClass=*/false)))
		{
			TemplateObjectName = FName(*RequestedTemplateName);
		}
		else
		{
			if (!RequestedTemplateName.IsEmpty())
			{
				ResolutionWarnings.Add(FString::Printf(
					TEXT("AddComponent: requested template_name '%s' already exists on %s; a unique name was generated instead."),
					*RequestedTemplateName, *Blueprint->GeneratedClass->GetName()));
			}
			// Inline mirror of UK2Node_AddComponent::MakeNewComponentTemplateName
			// (member is not DLL-exported; the class is MinimalAPI).
			int32& Counter = Blueprint->ComponentTemplateNameIndex.FindOrAdd(ComponentClass->GetFName());
			FString ComponentClassNameAsString = ComponentClass->GetName();
			if (ComponentClass->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
			{
				ComponentClassNameAsString.RemoveFromEnd(TEXT("_C"));
			}
			do
			{
				TemplateObjectName = FName(*FString::Printf(TEXT("%s%s-%d"),
					*UK2Node_AddComponent::ComponentTemplateNamePrefix, *ComponentClassNameAsString, Counter++));
				if (StaticFindObjectFast(ComponentClass, Blueprint->GeneratedClass, TemplateObjectName) != nullptr)
				{
					TemplateObjectName = NAME_None;
				}
			} while (TemplateObjectName == NAME_None);
		}

		UActorComponent* ComponentTemplate = NewObject<UActorComponent>(
			Blueprint->GeneratedClass, ComponentClass, TemplateObjectName,
			RF_ArchetypeObject | RF_Public | RF_Transactional);
		Blueprint->ComponentTemplates.Add(ComponentTemplate);

		if (UEdGraphPin* TemplateNamePin = AddCompNode->GetTemplateNamePinChecked())
		{
			TemplateNamePin->DefaultValue = ComponentTemplate->GetName();
		}

		AddCompNode->ReconstructNode();

		bNodeAlreadyAdded = true;
		NewNode = AddCompNode;
		NodeDescription = FString::Printf(TEXT("Add Component: %s (template %s)"),
			*ComponentClass->GetName(), *ComponentTemplate->GetName());
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Unsupported node type: %s. Use 'Generic' with 'class_name' parameter for custom node types."), *NodeType));
	}

	if (!IsValid(NewNode))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create node of type: %s"), *NodeType));
	}

	if (!bNodeAlreadyAdded)
	{
		// If auto-connect is enabled and we have a cursor node, calculate position relative to it
		if (bAutoConnect && Data->Cursor.FocusedNodeGuid.IsValid())
		{
			UEdGraphNode* CursorNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, Data->Cursor.FocusedNodeGuid);
			if (IsValid(CursorNode) && !bExplicitPosition)
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
			else if (IsValid(SwitchNode) && !SwitchNode->IsA<UK2Node_SwitchEnum>())
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
		if (IsValid(CursorNode))
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

					UEdGraphPin* FromPin = (CursorPin->Direction == EGPD_Output) ? CursorPin : TargetPin;
					UEdGraphPin* ToPin = (CursorPin->Direction == EGPD_Output) ? TargetPin : CursorPin;

					// TryCreateConnection returns false when the schema rejects the
					// link; report the failure honestly instead of claiming success --
					// the node add itself still succeeds either way.
					if (K2Schema->TryCreateConnection(FromPin, ToPin))
					{
						AutoConnectMessage = FString::Printf(TEXT("\nAuto-connected: %s -> %s"), *FromPin->PinName.ToString(), *ToPin->PinName.ToString());
					}
					else
					{
						const FPinConnectionResponse ConnResponse = K2Schema->CanCreateConnection(FromPin, ToPin);
						const FString ConnReason = ConnResponse.Message.IsEmpty()
							? FString(TEXT("schema rejected the connection"))
							: ConnResponse.Message.ToString();
						AutoConnectMessage = FString::Printf(
							TEXT("\nAuto-connect NOT made: %s -> %s (%s). Node was added; wire it manually via bp_connect_pins."),
							*FromPin->PinName.ToString(), *ToPin->PinName.ToString(), *ConnReason);
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
				if (LinkedAffPin && IsValid(LinkedAffPin->GetOwningNode()))
				{
					Data->LastOperationAffectedNodes.Add(LinkedAffPin->GetOwningNode()->NodeGuid);
				}
			}
		}
	}

	FToolResult AddNodeResult = BuildStateResponse(SessionId, Data);
	AddNodeResult.Warnings.Append(ResolutionWarnings);
	// Surface the new node's GUID directly so callers can chain follow-up ops (connect_pins,
	// set_node_property, ...) without regexing it out of the cursor/summary block.
	if (AddNodeResult.Data.IsValid())
	{
		AddNodeResult.Data->SetStringField(TEXT("created_node_guid"), NewNode->NodeGuid.ToString());
	}
	return AddNodeResult;
}

// ----------------------------------------------------------------------------
// hot-path metadata enrichment
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

FString ClaireonBlueprintGraphTool_AddNode::GetExampleUsage() const
{
    return TEXT(
        "bp_add_node session_id=\"...\" "
        "node_class=\"K2Node_CallFunction\" function=\"PrintString\" "
        "auto_connect_from_cursor=true");
}

FString ClaireonBlueprintGraphTool_AddNode::GetPatterns() const
{
    // Save-discipline guidance lives here (not GetFullDescription) so
    // tool_search deep-inspect can surface it under a dedicated `patterns`
    // field. ASCII only; no em-dashes.
    return TEXT(
        "## Common pitfalls\n"
        "\n"
        "Per the per-node cycle in .claude/areas/blueprint-editing.md, save "
        "every 1-3 add_node calls via bp_save to flush in-session edits to "
        "the asset and protect against editor-crash data loss.\n"
        "\n"
        "Authoring more than ~3 nodes? Use claireon.bp_apply_spec instead: "
        "one call creates all nodes, pin defaults, and connections "
        "atomically, instead of one round trip per node/pin/wire.\n"
        "\n"
        "## See also\n"
        "\n"
        "- claireon.bp_apply_spec -- batch/bulk authoring primitive; prefer "
        "it for any multi-node change\n"
        "- claireon.bp_connect_pins -- companion when auto_connect_from_cursor "
        "doesn't fit\n"
        "- claireon.bp_save -- per-node-cycle save discipline\n");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddNode::GetParameterTooltips() const
{
    TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
    T->SetStringField(TEXT("session_id"), TEXT("Session ID returned by bp_open or _create. Optional if asset_path is supplied."));
    T->SetStringField(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id; auto-opens the session)."));
    // node_type is the REQUIRED selector and had no tooltip at all, while node_class's
    // tooltip described node_type's job. node_class is the narrower CallFunction-only
    // override documented in GetInputSchema.
    T->SetStringField(TEXT("node_type"), TEXT("Node class alias or full name -- the required selector (e.g. CallFunction, Branch, Sequence, Cast, VariableGet, MacroInstance, or a K2Node_* class name). Fuzzy-resolved (drop the U prefix; partial matches allowed)."));
    T->SetStringField(TEXT("node_class"), TEXT("For CallFunction only: the exact UK2Node_CallFunction subclass to instantiate (e.g. K2Node_PromotableOperator, K2Node_CallArrayFunction). Overrides the class inferred from the function's metadata; leave unset unless replaying a specific source node."));
    T->SetStringField(TEXT("function_name"), TEXT("For CallFunction/AsyncAction/CallParentFunction/EventOverride/CreateDelegate: the function name to call. Resolved against UFUNCTION metadata; pair with function_class to disambiguate."));
    T->SetStringField(TEXT("auto_connect_from_cursor"), TEXT("If true, route the new node's exec pin through the cursor pin when compatible. Preferred over a follow-up bp_connect_pins call."));
    return T;
}

#undef LOCTEXT_NAMESPACE
