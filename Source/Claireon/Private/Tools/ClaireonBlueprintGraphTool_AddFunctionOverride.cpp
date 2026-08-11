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
#include "UObject/Interface.h"
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

// Named (not anonymous) so unity batching cannot collide this with a same-shaped
// helper in a sibling translation unit.
namespace ClaireonAddFnOverride_Internal
{
	/**
	 * True when an event override of FuncName anywhere in ParentBP's ubergraphs has
	 * something wired to its exec output.
	 *
	 * Matches on the event reference's member name rather than going through
	 * FBlueprintEditorUtils::FindOverrideForFunction, which requires naming the exact
	 * signature class the event was bound against. That bookkeeping varies with how
	 * the override was created, and getting it wrong silently reports "no body".
	 */
	static bool EventOverrideHasWiredBody(UBlueprint* ParentBP, FName FuncName)
	{
		for (UEdGraph* Graph : ParentBP->UbergraphPages)
		{
			if (!IsValid(Graph)) { continue; }
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_Event* EventNode = Cast<UK2Node_Event>(Node);
				if (!IsValid(EventNode)) { continue; }
				if (EventNode->EventReference.GetMemberName() != FuncName
					&& EventNode->CustomFunctionName != FuncName)
				{
					continue;
				}
				for (UEdGraphPin* Pin : EventNode->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output
						&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
						&& Pin->LinkedTo.Num() > 0)
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	/** True when the function-graph override of FuncName on ParentBP has more than scaffolding. */
	static bool FunctionGraphHasBody(UBlueprint* ParentBP, FName FuncName)
	{
		for (UEdGraph* Graph : ParentBP->FunctionGraphs)
		{
			if (!IsValid(Graph) || Graph->GetFName() != FuncName)
			{
				continue;
			}
			// More than the entry+result pair means real content was added.
			if (Graph->Nodes.Num() > 2)
			{
				return true;
			}
			// Two nodes can still be a body if entry actually flows into something.
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node);
				if (!IsValid(Entry)) { continue; }
				for (UEdGraphPin* Pin : Entry->Pins)
				{
					if (Pin && Pin->Direction == EGPD_Output
						&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec
						&& Pin->LinkedTo.Num() > 0)
					{
						return true;
					}
				}
			}
			return false;
		}
		return false;
	}

	/**
	 * Whether overriding TargetFunc as an event on Blueprint would shadow a real body
	 * somewhere up the parent chain.
	 *
	 * Two independent sources of a parent body, and either is enough:
	 *
	 * 1. A native implementation on the declaring class. FUNC_Native separates a
	 *    BlueprintNativeEvent (UHT emits a native thunk plus a <Name>_Implementation,
	 *    so a body exists) from a pure BlueprintImplementableEvent (no native
	 *    implementation at all). A C++ body cannot be introspected for emptiness
	 *    through reflection, so any native implementation counts -- a false positive
	 *    only costs a warning, a false negative silently drops behavior.
	 *
	 * 2. A Blueprint ancestor that already overrides the function with something
	 *    wired. Note this is NOT the same as "the function is declared on a
	 *    Blueprint": the common shape is a native BIE declaration that an
	 *    intermediate Blueprint overrides, so keying off TargetFunc's declaring
	 *    class alone would miss it. Walk the actual ancestry instead.
	 */
	static bool ParentImplementationHasBody(UBlueprint* Blueprint, const UFunction* TargetFunc)
	{
		if (!IsValid(TargetFunc))
		{
			return false;
		}

		if (TargetFunc->HasAnyFunctionFlags(FUNC_Native))
		{
			return true;
		}

		if (!IsValid(Blueprint))
		{
			return false;
		}

		const FName FuncFName = TargetFunc->GetFName();
		for (UClass* Ancestor = Blueprint->ParentClass; IsValid(Ancestor); Ancestor = Ancestor->GetSuperClass())
		{
			UBlueprint* AncestorBP = Cast<UBlueprint>(Ancestor->ClassGeneratedBy);
			if (!IsValid(AncestorBP))
			{
				continue;
			}
			if (EventOverrideHasWiredBody(AncestorBP, FuncFName)
				|| FunctionGraphHasBody(AncestorBP, FuncFName))
			{
				return true;
			}
		}
		return false;
	}
} // namespace ClaireonAddFnOverride_Internal


FString ClaireonBlueprintGraphTool_AddFunctionOverride::GetOperation() const { return TEXT("add_function_override"); }

FString ClaireonBlueprintGraphTool_AddFunctionOverride::GetDescription() const
{
    return TEXT("Create a function-override graph for a BlueprintNativeEvent or BlueprintImplementableEvent in the open editing session. Requires open session_id from bp_open (or pass asset_path to auto-open). Transactional. The override target must be declared on the parent class or a UFUNCTION-marked interface. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_AddFunctionOverride::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("function_name"), TEXT("Name of the parent function to override."), true);
    Builder.AddString(TEXT("interface_class"), TEXT("Optional interface class path if overriding an interface function."));
    Builder.AddBoolean(TEXT("force_function_graph"), TEXT("Create the override as a FUNCTION graph even when the signature could be placed as an event node (UPARAM(ref) signatures pass the event check; replay callers pass true when the source implements a function graph)."));
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
	if (!IsValid(Blueprint))
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// 1b. interface_class. The schema has declared it since this tool shipped and
	// no code ever read it: resolution is parent-class-only, so a caller who
	// named an interface got a parent lookup and, when it missed, an error that
	// never mentioned the interface they had asked about. Interface functions are
	// not overridable through this tool -- bp_add_interface authors a graph per
	// interface function, and step 4c below already redirects to it -- so the
	// honest answer is an error that names interface_class, never a silent
	// parent-only miss.
	FString InterfaceClassName;
	if (Params->TryGetStringField(TEXT("interface_class"), InterfaceClassName) && !InterfaceClassName.IsEmpty())
	{
		ClaireonNameResolver::FNameResolveResult IfaceResult;
		UClass* InterfaceClass = ClaireonNameResolver::ResolveClassName(
			InterfaceClassName, UInterface::StaticClass(), IfaceResult);

		if (!IsValid(InterfaceClass))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("interface_class '%s' could not be resolved to an interface: %s"),
				*InterfaceClassName,
				IfaceResult.Error.IsEmpty() ? TEXT("no matching interface class") : *IfaceResult.Error));
		}

		const FBPInterfaceDescription* Implemented = Blueprint->ImplementedInterfaces.FindByPredicate(
			[InterfaceClass](const FBPInterfaceDescription& Desc)
			{
				return Desc.Interface == InterfaceClass;
			});

		if (Implemented == nullptr)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Blueprint does not implement interface_class '%s'. Add it with bp_add_interface first; "
					 "that also creates the function graph for each interface function."),
				*InterfaceClass->GetName()));
		}

		return MakeErrorResult(FString::Printf(
			TEXT("'%s' is an interface function on '%s', which this Blueprint already implements. "
				 "bp_add_interface created a graph per interface function -- edit that graph "
				 "(bp_open + bp_set_graph) instead of creating an override."),
			*FunctionName, *InterfaceClass->GetName()));
	}

	// 2. Resolve the function on the parent class
	UClass* ParentClass = Blueprint->ParentClass;
	ClaireonNameResolver::FNameResolveResult FuncResult;
	UFunction* TargetFunc = IsValid(ParentClass)
		? ClaireonNameResolver::ResolveFunctionName(ParentClass, FunctionName, FuncResult)
		: nullptr;

	if (!IsValid(TargetFunc))
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
	if (IsValid(ExistingEventOverride))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Override for '%s' already exists as event node (GUID: %s)"),
			*TargetFunc->GetName(), *ExistingEventOverride->NodeGuid.ToString()));
	}

	// 4b. Check for existing function graph with the function's name
	FName FuncFName = TargetFunc->GetFName();
	for (UEdGraph* ExistingGraph : Blueprint->FunctionGraphs)
	{
		if (IsValid(ExistingGraph) && ExistingGraph->GetFName() == FuncFName)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Override for '%s' already exists as function graph"),
				*TargetFunc->GetName()));
		}
	}

	// 4c. Check implemented-interface graphs. bp_add_interface already creates a
	// graph per interface function with outputs; creating an override on top of
	// it authors a duplicate event in a non-event graph and makes the interface
	// graph unreachable by name.
	for (const FBPInterfaceDescription& Iface : Blueprint->ImplementedInterfaces)
	{
		for (UEdGraph* IfaceGraph : Iface.Graphs)
		{
			if (IsValid(IfaceGraph) && IfaceGraph->GetFName() == FuncFName)
			{
				return MakeErrorResult(FString::Printf(
					TEXT("'%s' is implemented by interface '%s' on this Blueprint; edit that interface graph instead of creating an override"),
					*TargetFunc->GetName(),
					IsValid(Iface.Interface) ? *Iface.Interface->GetName() : TEXT("<unknown>")));
			}
		}
	}

	// 5. Branch the same way the editor's Override flow does: a function with ANY
	// output/return/reference parameter must be overridden as a FUNCTION GRAPH
	// (entry/result pair); only output-less functions may be placed as event nodes.
	// Branching on FUNC_Native here is wrong twice over: a parent-BP function is
	// never native (so BP-parent overrides with returns landed as event nodes and
	// failed to compile), and native-ness says nothing about the signature.
	// Decide against the override class's DECLARATION of the function, not the
	// implementing parent's copy: placement-forcing metadata on a native
	// interface declaration is not duplicated onto the class's version (CHA
	// RequestCamera repro -- the parent copy placed as an event, the source
	// implements a function graph).
	UFunction* OverrideFunc = nullptr;
	UClass* const OverrideFuncClass = FBlueprintEditorUtils::GetOverrideFunctionClass(Blueprint, FuncFName, &OverrideFunc);
	const UFunction* PlacementFunc = IsValid(OverrideFunc) ? OverrideFunc : TargetFunc;
	// force_function_graph: caller knows the override must be a FUNCTION graph
	// (e.g. replaying a source asset whose functions[] list places it there);
	// UPARAM(ref) signatures pass FunctionCanBePlacedAsEvent, so the automatic
	// decision alone cannot reproduce a function-graph implementation of them.
	bool bForceFunctionGraph = false;
	Params->TryGetBoolField(TEXT("force_function_graph"), bForceFunctionGraph);
	const bool bAsFunctionGraph = bForceFunctionGraph || !UEdGraphSchema_K2::FunctionCanBePlacedAsEvent(PlacementFunc);

	// Set on the event path; surfaced as Data.node_guid so the caller can address
	// the node it just created without a follow-up get_graph.
	FGuid CreatedEventNodeGuid;

	if (bAsFunctionGraph)
	{
		// === Function-graph path: mirror SMyBlueprint::ImplementFunction ===
		// AddFunctionGraph with bIsUserCreated=false and the override class binds the
		// entry/result pair to the parent signature by graph name; manually binding
		// the entry node afterwards produces malformed override graphs.

		if (!IsValid(OverrideFunc) || !IsValid(OverrideFuncClass))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Function '%s' is not overridable on this Blueprint (no override class found)"),
				*TargetFunc->GetName()));
		}

		// Create the graph named EXACTLY after the function so the signature binds.
		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			Blueprint,
			FuncFName,
			UEdGraph::StaticClass(),
			UEdGraphSchema_K2::StaticClass());

		if (!IsValid(NewGraph))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Failed to create function graph for '%s'"), *TargetFunc->GetName()));
		}

		FBlueprintEditorUtils::AddFunctionGraph(Blueprint, NewGraph, /*bIsUserCreated=*/false, OverrideFuncClass);
		NewGraph->Modify();

		// Get the entry/result nodes AddFunctionGraph created
		UK2Node_FunctionEntry* EntryNode = nullptr;
		{
			TArray<UK2Node_FunctionEntry*> EntryNodes;
			NewGraph->GetNodesOfClass(EntryNodes);
			if (EntryNodes.Num() > 0)
			{
				EntryNode = EntryNodes[0];
			}
		}

		if (!IsValid(EntryNode))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Failed to find entry node in new function graph for '%s'"), *TargetFunc->GetName()));
		}

		UK2Node_FunctionResult* ResultNode = nullptr;
		{
			TArray<UK2Node_FunctionResult*> ResultNodes;
			NewGraph->GetNodesOfClass(ResultNodes);
			if (ResultNodes.Num() > 0)
			{
				ResultNode = ResultNodes[0];
			}
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
			TEXT("Created function override graph for '%s'. Session graph switched to '%s'."),
			*TargetFunc->GetName(), *NewGraph->GetName());

		// Track affected nodes for response_mode="changed"
		Data->LastOperationAffectedNodes.Add(EntryNode->NodeGuid);
		if (IsValid(ResultNode))
		{
			Data->LastOperationAffectedNodes.Add(ResultNode->NodeGuid);
		}
	}
	else
	{
		// === Event path (no output params): create UK2Node_Event in EventGraph ===

		// Event overrides live in event graphs. The session may be pointed at a
		// FUNCTION graph (e.g. mid-way through creating functions); dropping the
		// event there compiles with 'Event node registers net in a non-event
		// graph'. Route to the blueprint's primary ubergraph instead.
		UEdGraph* Graph = Data->Graph.Get();
		if (!IsValid(Graph) || GetDefault<UEdGraphSchema_K2>()->GetGraphType(Graph) != GT_Ubergraph)
		{
			Graph = Blueprint->UbergraphPages.Num() > 0 ? Blueprint->UbergraphPages[0] : nullptr;
		}
		if (!IsValid(Graph))
		{
			return MakeErrorResult(TEXT("No event graph available for the event override"));
		}

		UK2Node_Event* EventNode = NewObject<UK2Node_Event>(Graph);
		EventNode->EventReference.SetExternalMember(TargetFunc->GetFName(), ParentClass);
		EventNode->bOverrideFunction = true;
		// AddNode does not assign one; a guid-less node breaks callers that map
		// the response (and get_graph later assigns an arbitrary one).
		EventNode->CreateNewGuid();

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
		CreatedEventNodeGuid = EventNode->NodeGuid;
	}

	// Include resolution note if applicable
	if (!FuncResult.ResolutionNote.IsEmpty())
	{
		Data->Cursor.LastOperationStatus += FString::Printf(TEXT(" [%s]"), *FuncResult.ResolutionNote);
	}

	FToolResult OverrideResult = BuildStateResponse(SessionId, Data);

	// Structured outcome: which of the two shapes did the caller actually get?
	// Previously only the free-text status line said so. graph_name (already on the
	// response) is the new function graph for function_override, and the session's
	// current graph for event_override.
	if (OverrideResult.Data.IsValid())
	{
		OverrideResult.Data->SetStringField(TEXT("kind"),
			bAsFunctionGraph ? TEXT("function_override") : TEXT("event_override"));
		if (!bAsFunctionGraph && CreatedEventNodeGuid.IsValid())
		{
			OverrideResult.Data->SetStringField(TEXT("node_guid"),
				CreatedEventNodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		}
	}

	// An event override silently shadows a parent implementation: the parent body
	// does not run unless the caller wires an explicit Parent call. Warn when the
	// parent actually has a body to lose.
	if (!bAsFunctionGraph
		&& ClaireonAddFnOverride_Internal::ParentImplementationHasBody(Blueprint, TargetFunc))
	{
		const FString FuncDisplayName = TargetFunc->GetName();
		OverrideResult.Warnings.Add(FString::Printf(
			TEXT("Parent '%s' has a non-empty body. Event override was created; parent body will NOT run "
				 "unless you add a 'Parent: %s' call node (bp_add_node node_type='CallParentFunction', "
				 "function_name='%s')."),
			*FuncDisplayName, *FuncDisplayName, *FuncDisplayName));
	}

	return OverrideResult;
}

#undef LOCTEXT_NAMESPACE
