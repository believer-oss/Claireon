// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_EditBlueprintGraph.h"
#include "Tools/ClaireonSpecApplicator_Blueprint.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "ClaireonBlueprintHelpers.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
// K2Node type includes for node creation
#include "K2Node_CallFunction.h"
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
// Dynamic pin and Switch node includes
#include "K2Node_AddPinInterface.h"
#include "K2Node_Switch.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_SwitchString.h"
#include "K2Node_SwitchName.h"
#include "K2Node_SwitchEnum.h"
#include "K2Node_ForEachElementInEnum.h"
#include "K2Node_DoOnceMultiInput.h"
// Delegate binding node includes
#include "K2Node_AddDelegate.h"
#include "K2Node_RemoveDelegate.h"
#include "K2Node_ClearDelegate.h"
#include "K2Node_CallDelegate.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "Engine/TimelineTemplate.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveVector.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "EdGraphUtilities.h"
#include "Dom/JsonObject.h"
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
#include "GameplayTagContainer.h"
#include "GameplayTagsManager.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"

// UE 5.7 changed NodePosX/Y from float to int32, adding getter/setter accessors.
// Use these compat macros so the code compiles on both 5.5 and 5.7+.
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 7
#define CLAIREON_GET_NODE_POS_X(Node) (Node)->GetNodePosX()
#define CLAIREON_GET_NODE_POS_Y(Node) (Node)->GetNodePosY()
#define CLAIREON_SET_NODE_POS_X(Node, Val) (Node)->SetNodePosX(Val)
#define CLAIREON_SET_NODE_POS_Y(Node, Val) (Node)->SetNodePosY(Val)
#else
#define CLAIREON_GET_NODE_POS_X(Node) (Node)->NodePosX
#define CLAIREON_GET_NODE_POS_Y(Node) (Node)->NodePosY
#define CLAIREON_SET_NODE_POS_X(Node, Val) (Node)->NodePosX = (Val)
#define CLAIREON_SET_NODE_POS_Y(Node, Val) (Node)->NodePosY = (Val)
#endif

#define LOCTEXT_NAMESPACE "ClaireonTool_EditBlueprintGraph"

// Using statements
using FToolResult = IClaireonTool::FToolResult;

TMap<FString, FBlueprintEditToolData> ClaireonTool_EditBlueprintGraph::ToolData;
bool ClaireonTool_EditBlueprintGraph::bDelegateRegistered = false;

FString ClaireonTool_EditBlueprintGraph::GetName() const
{
	return TEXT("claireon.blueprint_edit_graph");
}

FString ClaireonTool_EditBlueprintGraph::GetDescription() const
{
	return TEXT("Session-based Blueprint graph editor. Use cursor-based operations to add nodes, make connections, and navigate. Supports standard Blueprints, Animation Blueprints, Widget Blueprints, and Function Libraries. Start with 'open', then mutate, then 'save'/'close'.");
}

FString ClaireonTool_EditBlueprintGraph::GetFullDescription() const
{
	return TEXT("Interactively edit a Blueprint graph using a cursor-based model. Each operation returns the updated graph state and cursor position. Supports standard Blueprints, Animation Blueprints, Widget Blueprints, and Function Libraries.\n\n"
				"RECOMMENDED WORKFLOW — build graphs incrementally, one node at a time:\n"
				"  1. 'open' the Blueprint\n"
				"  2. For EACH node: 'add_node' with auto_connect_from_cursor=true (creates + wires + advances cursor)\n"
				"     - 'set_pin_value' if defaults needed, 'connect_pins' only for extra data wires\n"
				"  3. 'save' every 1-3 nodes\n"
				"  4. After all nodes: use claireon.blueprint_format_graph (separate tool) for layout\n"
				"  5. 'save', 'compile', 'close'\n"
				"Do NOT batch-create all nodes then connect them afterward — you will lose track of GUIDs.\n"
				"For branching: use 'select_pin' to move cursor to the branch point, then continue adding.\n\n"
				"response_mode parameter (default: \"changed\"):\n"
				"- \"changed\" — pin-level diff showing only nodes whose connections changed (default, lowest token cost)\n"
				"- \"full\"    — full graph state (JSON + T3D), same as original behavior\n"
				"- \"status\"  — brief status line only (equivalent to deprecated suppress_output=true)\n"
				"The 'open' operation always returns the full graph at exec detail level for initial orientation,\n"
				"regardless of response_mode. response_mode applies to all subsequent mutation operations.\n"
				"suppress_output is deprecated — use response_mode=\"status\" instead.\n\n"
				"add_node operation supports these node_type values:\n"
				"Basic Nodes:\n"
				"- CallFunction (requires: function_name, optional: function_class)\n"
				"- VariableGet (requires: variable_name)\n"
				"- VariableSet (requires: variable_name)\n"
				"- CustomEvent (requires: event_name)\n"
				"- Knot (reroute node)\n"
				"- Comment (optional: comment_text)\n"
				"Control Flow:\n"
				"- Branch (if-then-else)\n"
				"- Sequence (execution sequence)\n"
				"- Select (switch/select)\n"
				"- SwitchInteger, SwitchString, SwitchName\n"
				"- SwitchEnum (requires: enum_type)\n"
				"- ForEachElementInEnum (requires: enum_type)\n"
				"- DoOnceMultiInput\n"
				"Macros (StandardMacros library):\n"
				"- ForEachLoop, ForEachLoopWithBreak, ForLoop, ForLoopWithBreak, WhileLoop\n"
				"- DoOnce, DoN, FlipFlop, Gate, MultiGate, IsValid\n"
				"- Macro (requires: macro_name, optional: macro_library_path)\n"
				"Objects & Data:\n"
				"- Cast (requires: target_class)\n"
				"- SpawnActor (requires: actor_class)\n"
				"- MakeStruct (requires: struct_type)\n"
				"- BreakStruct (requires: struct_type)\n"
				"Collections:\n"
				"- MakeArray, MakeSet, MakeMap\n"
				"- GetArrayItem\n"
				"Delegates:\n"
				"- AddDelegate (requires: delegate_name, optional: target_class) - Bind event to multicast delegate\n"
				"- RemoveDelegate (requires: delegate_name, optional: target_class) - Unbind event from delegate\n"
				"- ClearDelegate (requires: delegate_name, optional: target_class) - Clear all delegate bindings\n"
				"- CallDelegate (requires: delegate_name, optional: target_class) - Fire/broadcast delegate\n"
				"- CreateDelegate (requires: function_name) - Create delegate from function reference (pure node)\n"
				"- AssignDelegate (requires: delegate_name, optional: target_class, event_name) - Bind + auto-create custom event\n"
				"Advanced:\n"
				"- Generic (requires: class_name, optional: node_properties) - Create any K2Node by class name\n"
				"All dynamic-pin nodes support optional num_extra_pins parameter (max 50).\n\n"
				"add_pin operation: Add dynamic pins to Sequence, MakeArray, MakeSet, MakeMap, Select, Switch*, DoOnceMultiInput.\n"
				"  Params: node_guid (required), count (optional, default 1), pin_value (optional, for SwitchString/SwitchName case value).\n"
				"  SwitchEnum does not support add_pin (pins fixed to enum entries).\n\n"
				"remove_pin operation: Remove dynamic pins. Params: node_guid, pin_name or pin_index.\n\n"
				"split_pin / recombine_pin operations: Split struct pins into components or recombine them.\n"
				"  Params: node_guid, pin_name.\n\n"
				"connect_pins operation accepts:\n"
				"- Node identification: source/target_node_guid OR source/target_node_title\n"
				"- Pin identification: source/target_pin_name (required)\n"
				"- Optional: source/target_pin_direction ('input' or 'output') for disambiguation\n"
				"- GUIDs are preferred when multiple nodes share the same title\n\n"
				"add_variable operation:\n"
				"  Required: variable_name, variable_type\n"
				"  Optional: default_value, flags[], clear_flags[], category, tooltip, display_name,\n"
				"    replication ('None'|'Replicated'|'RepNotify'), rep_notify_func, replication_condition,\n"
				"    metadata (object of key/value pairs, e.g. UIMin, UIMax, ClampMin, ClampMax, Units, EditCondition)\n\n"
				"set_variable_properties operation: Modify properties of an existing variable.\n"
				"  Required: variable_name\n"
				"  Optional: flags[], clear_flags[], category, tooltip, display_name,\n"
				"    replication ('None'|'Replicated'|'RepNotify'), rep_notify_func, replication_condition,\n"
				"    metadata (object of key/value pairs)\n"
				"  flags[] values: BlueprintReadOnly, BlueprintReadWrite, EditAnywhere, EditDefaultsOnly,\n"
				"    EditInstanceOnly, VisibleAnywhere, Transient, Config, SaveGame, Interp, ExposeOnSpawn,\n"
				"    Net, Replicated, RepNotify, AdvancedDisplay, AssetRegistrySearchable, SimpleDisplay,\n"
				"    DisableEditOnTemplate\n"
				"  clear_flags[] uses same values but removes them.\n"
				"  replication field takes precedence over Net/RepNotify in flags[].\n\n"
				"Recovery workflow for locked assets:\n"
				"  1. Use the shared list_sessions / release_all tools (Stage 013) to inspect and release locks\n"
				"  2. If the Blueprint has unsaved changes, save manually first before force-releasing\n\n"
				"Component management operations (session-based, require an Actor Blueprint):\n"
				"- remove_component: Remove a component and promote its children.\n"
				"    Params: component_name (required)\n"
				"- reparent_component: Move a component to a new parent or to root level.\n"
				"    Params: component_name (required), parent_component (optional, omit to move to root)\n"
				"- rename_component: Rename a component variable.\n"
				"    Params: component_name (required), new_name (required)\n"
				"- set_root_component: Designate a component as the new scene root.\n"
				"    Params: component_name (required)\n"
				"- get_component_details: Get detailed info about a component including hierarchy and properties.\n"
				"    Params: component_name (required), include_defaults (optional boolean, default false)\n\n"
				"add_function_override operation:\n"
				"  Creates the correct override graph for a parent class function.\n"
				"  Required: function_name (name of the parent class function to override)\n"
				"  - For BlueprintNativeEvent functions: creates a new function graph in FunctionGraphs\n"
				"    with entry/result nodes matching the parent signature. The session's active graph\n"
				"    switches to the new function graph automatically.\n"
				"  - For BlueprintImplementableEvent functions: creates a UK2Node_Event in EventGraph\n"
				"    (same as EventOverride node_type). Session stays on EventGraph.\n"
				"  Use this instead of add_node(node_type=EventOverride) for all function overrides.\n"
				"  Returns error if the override already exists.\n\n"
				"Declarative: apply_spec - Apply a declarative JSON specification to create/modify the asset atomically. "
				"Accepts: asset_path (string, required), spec (object, required), session_id (string, optional).");
}

TSharedPtr<FJsonObject> ClaireonTool_EditBlueprintGraph::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// operation - required string (e.g. "open", "add_node", "connect_pins", "save", "close")
	TSharedPtr<FJsonObject> OpProp = MakeShared<FJsonObject>();
	OpProp->SetStringField(TEXT("type"), TEXT("string"));
	OpProp->SetStringField(TEXT("description"), TEXT("The operation to perform: open, create, add_node (supports optional position: {x,y}), remove_node, connect_pins, disconnect_pins, set_pin_value, move_node (requires node_guid + position: {x,y}), add_pin, remove_pin, split_pin, recombine_pin, add_variable, set_variable_properties, add_function_override, save, close, list_graphs, reconstruct_node, set_gameplay_tags, remove_component, reparent_component, rename_component, set_root_component, get_component_details, apply_spec, etc."));
	Properties->SetObjectField(TEXT("operation"), OpProp);

	// asset_path - required for open/create
	TSharedPtr<FJsonObject> PathProp = MakeShared<FJsonObject>();
	PathProp->SetStringField(TEXT("type"), TEXT("string"));
	PathProp->SetStringField(TEXT("description"), TEXT("Unreal content path of the Blueprint to edit (e.g., /Game/Characters/BP_Enemy). Must start with /Game/. Required for 'open' and 'create' operations."));
	Properties->SetObjectField(TEXT("asset_path"), PathProp);

	// session_id - required for all operations except open/create
	TSharedPtr<FJsonObject> SessionProp = MakeShared<FJsonObject>();
	SessionProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionProp->SetStringField(TEXT("description"), TEXT("Session ID returned by the 'open' operation. Required for all operations except 'open' and 'create'."));
	Properties->SetObjectField(TEXT("session_id"), SessionProp);

	// timeout_minutes - optional, for open/create
	TSharedPtr<FJsonObject> TimeoutProp = MakeShared<FJsonObject>();
	TimeoutProp->SetStringField(TEXT("type"), TEXT("number"));
	TimeoutProp->SetStringField(TEXT("description"), TEXT("Session timeout in minutes (default: 60). Use higher values for known-long workflows."));
	Properties->SetObjectField(TEXT("timeout_minutes"), TimeoutProp);

	// graph_name - optional
	TSharedPtr<FJsonObject> GraphProp = MakeShared<FJsonObject>();
	GraphProp->SetStringField(TEXT("type"), TEXT("string"));
	GraphProp->SetStringField(TEXT("description"), TEXT("Name of the graph to edit (default: EventGraph). Use get_blueprint_properties to see available graphs."));
	Properties->SetObjectField(TEXT("graph_name"), GraphProp);

	// response_mode - optional
	TSharedPtr<FJsonObject> RespProp = MakeShared<FJsonObject>();
	RespProp->SetStringField(TEXT("type"), TEXT("string"));
	RespProp->SetStringField(TEXT("description"), TEXT("Output detail level: 'changed' (default, pin-level diff), 'full' (complete graph state), 'status' (brief status line only)."));
	Properties->SetObjectField(TEXT("response_mode"), RespProp);

	// variable_name - required for add_variable, set_variable_properties
	TSharedPtr<FJsonObject> VarNameProp = MakeShared<FJsonObject>();
	VarNameProp->SetStringField(TEXT("type"), TEXT("string"));
	VarNameProp->SetStringField(TEXT("description"), TEXT("Variable name. Required for add_variable and set_variable_properties."));
	Properties->SetObjectField(TEXT("variable_name"), VarNameProp);

	// variable_type - required for add_variable
	TSharedPtr<FJsonObject> VarTypeProp = MakeShared<FJsonObject>();
	VarTypeProp->SetStringField(TEXT("type"), TEXT("string"));
	VarTypeProp->SetStringField(TEXT("description"), TEXT("Variable type string (e.g. 'float', 'int', 'bool', 'Vector', 'Array<Actor>'). Required for add_variable."));
	Properties->SetObjectField(TEXT("variable_type"), VarTypeProp);

	// category - optional for add_variable, set_variable_properties
	TSharedPtr<FJsonObject> CatProp = MakeShared<FJsonObject>();
	CatProp->SetStringField(TEXT("type"), TEXT("string"));
	CatProp->SetStringField(TEXT("description"), TEXT("Variable category (e.g. 'Combat|Stats'). Used by add_variable and set_variable_properties."));
	Properties->SetObjectField(TEXT("category"), CatProp);

	// tooltip - optional
	TSharedPtr<FJsonObject> TooltipProp = MakeShared<FJsonObject>();
	TooltipProp->SetStringField(TEXT("type"), TEXT("string"));
	TooltipProp->SetStringField(TEXT("description"), TEXT("Tooltip text for the variable. Used by add_variable and set_variable_properties."));
	Properties->SetObjectField(TEXT("tooltip"), TooltipProp);

	// display_name - optional
	TSharedPtr<FJsonObject> DisplayNameProp = MakeShared<FJsonObject>();
	DisplayNameProp->SetStringField(TEXT("type"), TEXT("string"));
	DisplayNameProp->SetStringField(TEXT("description"), TEXT("Display name override for the variable. Used by add_variable and set_variable_properties."));
	Properties->SetObjectField(TEXT("display_name"), DisplayNameProp);

	// replication - optional
	TSharedPtr<FJsonObject> ReplicationProp = MakeShared<FJsonObject>();
	ReplicationProp->SetStringField(TEXT("type"), TEXT("string"));
	ReplicationProp->SetStringField(TEXT("description"), TEXT("Replication mode: 'None', 'Replicated', or 'RepNotify'. Takes precedence over Net/RepNotify in flags[]."));
	Properties->SetObjectField(TEXT("replication"), ReplicationProp);

	// rep_notify_func - optional
	TSharedPtr<FJsonObject> RepFuncProp = MakeShared<FJsonObject>();
	RepFuncProp->SetStringField(TEXT("type"), TEXT("string"));
	RepFuncProp->SetStringField(TEXT("description"), TEXT("RepNotify function name (only with replication='RepNotify'). Defaults to OnRep_VarName."));
	Properties->SetObjectField(TEXT("rep_notify_func"), RepFuncProp);

	// replication_condition - optional
	TSharedPtr<FJsonObject> RepCondProp = MakeShared<FJsonObject>();
	RepCondProp->SetStringField(TEXT("type"), TEXT("string"));
	RepCondProp->SetStringField(TEXT("description"), TEXT("Replication condition: COND_None, COND_OwnerOnly, COND_SkipOwner, COND_SimulatedOnly, etc."));
	Properties->SetObjectField(TEXT("replication_condition"), RepCondProp);

	// flags - optional array
	TSharedPtr<FJsonObject> FlagsProp = MakeShared<FJsonObject>();
	FlagsProp->SetStringField(TEXT("type"), TEXT("array"));
	FlagsProp->SetStringField(TEXT("description"), TEXT("Property flags to SET (additive). Values: BlueprintReadOnly, BlueprintReadWrite, EditAnywhere, EditDefaultsOnly, EditInstanceOnly, VisibleAnywhere, Transient, Config, SaveGame, Interp, ExposeOnSpawn, Net, Replicated, RepNotify, AdvancedDisplay, AssetRegistrySearchable, SimpleDisplay, DisableEditOnTemplate."));
	Properties->SetObjectField(TEXT("flags"), FlagsProp);

	// clear_flags - optional array
	TSharedPtr<FJsonObject> ClearFlagsProp = MakeShared<FJsonObject>();
	ClearFlagsProp->SetStringField(TEXT("type"), TEXT("array"));
	ClearFlagsProp->SetStringField(TEXT("description"), TEXT("Property flags to CLEAR. Same values as flags[]."));
	Properties->SetObjectField(TEXT("clear_flags"), ClearFlagsProp);

	// metadata - optional object
	TSharedPtr<FJsonObject> MetadataProp = MakeShared<FJsonObject>();
	MetadataProp->SetStringField(TEXT("type"), TEXT("object"));
	MetadataProp->SetStringField(TEXT("description"), TEXT("Key/value metadata pairs. Common keys: UIMin, UIMax, ClampMin, ClampMax, Units, EditCondition, Bitmask, BitmaskEnum."));
	Properties->SetObjectField(TEXT("metadata"), MetadataProp);

	// new_name - required for rename_component
	TSharedPtr<FJsonObject> NewNameProp = MakeShared<FJsonObject>();
	NewNameProp->SetStringField(TEXT("type"), TEXT("string"));
	NewNameProp->SetStringField(TEXT("description"), TEXT("New variable name for the component. Required for rename_component. Must be a valid C++ identifier."));
	Properties->SetObjectField(TEXT("new_name"), NewNameProp);

	// include_defaults - optional for get_component_details
	TSharedPtr<FJsonObject> IncludeDefaultsProp = MakeShared<FJsonObject>();
	IncludeDefaultsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeDefaultsProp->SetStringField(TEXT("description"), TEXT("When true, get_component_details returns all editable properties. When false (default), returns only properties that differ from class defaults."));
	Properties->SetObjectField(TEXT("include_defaults"), IncludeDefaultsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}
FToolResult ClaireonTool_EditBlueprintGraph::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Get operation
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation))
	{
		return MakeErrorResult(TEXT("Missing 'operation' field"));
	}

	// All parameters are read from the top-level Arguments object (flat schema).
	// Legacy callers that nested params in a "params" sub-object are no longer supported.
	TSharedPtr<FJsonObject> Params = Arguments;

	// Check suppress_output flag (deprecated — use response_mode instead)
	bool bSuppressOutput = false;
	Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);

	// Determine response_mode; supersedes suppress_output
	FString ResponseMode = TEXT("changed"); // default
	Arguments->TryGetStringField(TEXT("response_mode"), ResponseMode);
	// suppress_output backward-compat: if response_mode was not explicitly set
	// (still "changed" after the TryGet) but suppress_output=true, treat as "status"
	if (bSuppressOutput && !Arguments->HasField(TEXT("response_mode")))
	{
		ResponseMode = TEXT("status");
	}

	// Route to operation handler
	// Note: open/create always return full output since the caller needs the session_id
	if (Operation == TEXT("open"))
	{
		return Operation_Open(Params);
	}
	else if (Operation == TEXT("create"))
	{
		return Operation_Create(Params);
	}
	else if (Operation == TEXT("list_graphs"))
	{
		return Operation_ListGraphs(Params);
	}
	else if (Operation == TEXT("apply_spec"))
	{
		return Operation_ApplySpec(Params);
	}
	else if (Operation == TEXT("remove_node"))
	{
		FString SessionId;
		if (Params->TryGetStringField(TEXT("session_id"), SessionId))
		{
			FMCPSession* MgrSession = FClaireonSessionManager::Get().FindSession(SessionId);
			if (!MgrSession)
				return MakeErrorResult(TEXT("Session not found or expired"));
			FClaireonSessionManager::Get().TouchSession(SessionId);

			FBlueprintEditToolData* Data = ToolData.Find(SessionId);
			if (!Data)
				return MakeErrorResult(TEXT("Tool data not found for session"));
			Data->bSuppressOutput = bSuppressOutput;
			Data->ResponseMode = ResponseMode;
			Data->LastOperationAffectedNodes.Empty();
			// Build pre-op pin snapshot
			Data->PreOpPinConnections.Empty();
			if (UEdGraph* SnapGraph = Data->Graph.Get())
			{
				for (UEdGraphNode* SnapNode : SnapGraph->Nodes)
				{
					if (!SnapNode)
					{
						continue;
					}
					TMap<FName, TArray<FString>> PinConns;
					for (UEdGraphPin* SnapPin : SnapNode->Pins)
					{
						if (!SnapPin)
						{
							continue;
						}
						TArray<FString> ConnectedTo;
						for (UEdGraphPin* LinkedPin : SnapPin->LinkedTo)
						{
							if (LinkedPin && LinkedPin->GetOwningNode())
							{
								ConnectedTo.Add(LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
							}
						}
						PinConns.Add(SnapPin->PinName, ConnectedTo);
					}
					Data->PreOpPinConnections.Add(SnapNode->NodeGuid, PinConns);
				}
			}
			FToolResult RemoveResult = Operation_RemoveNode(SessionId, Data, Params);
			if (!RemoveResult.bIsError && Data->ResponseMode == TEXT("changed") && Data->LastOperationAffectedNodes.IsEmpty())
			{
				UE_LOG(LogClaireon, Warning,
					TEXT("response_mode=changed: no affected nodes recorded after mutation op 'remove_node' — check handler"));
			}
			return RemoveResult;
		}
		else
		{
			return Operation_RemoveNodeStateless(Params);
		}
	}
	else if (Operation == TEXT("reconstruct_node"))
	{
		FString SessionId;
		if (Params->TryGetStringField(TEXT("session_id"), SessionId))
		{
			FMCPSession* MgrSession = FClaireonSessionManager::Get().FindSession(SessionId);
			if (!MgrSession)
				return MakeErrorResult(TEXT("Session not found or expired"));
			FClaireonSessionManager::Get().TouchSession(SessionId);

			FBlueprintEditToolData* Data = ToolData.Find(SessionId);
			if (!Data)
				return MakeErrorResult(TEXT("Tool data not found for session"));
			return Operation_ReconstructNode(SessionId, Data, Params);
		}
		else
		{
			return Operation_ReconstructNodeStateless(Params);
		}
	}
	else if (Operation == TEXT("set_gameplay_tags"))
	{
		return Operation_SetGameplayTags(Params);
	}
	else
	{
		// All other operations require a session_id
		FString SessionId;
		if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId))
		{
			return MakeErrorResult(FString::Printf(TEXT("Missing 'session_id' for operation '%s'. First call open(asset_path='/Game/...') to get a session_id, then pass it to subsequent operations."), *Operation));
		}

		// Find the session in the manager
		FMCPSession* MgrSession = FClaireonSessionManager::Get().FindSession(SessionId);
		if (!MgrSession)
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid or expired session_id: %s"), *SessionId));
		}

		// Find tool-specific data
		FBlueprintEditToolData* Data = ToolData.Find(SessionId);
		if (!Data)
		{
			return MakeErrorResult(FString::Printf(TEXT("Tool data not found for session_id: %s"), *SessionId));
		}

		// Update access time and set response flags for BuildStateResponse
		FClaireonSessionManager::Get().TouchSession(SessionId);
		Data->bSuppressOutput = bSuppressOutput;
		Data->ResponseMode = ResponseMode;

		// Clear affected-nodes set at the start of each operation dispatch
		Data->LastOperationAffectedNodes.Empty();

		// Build pre-operation pin connection snapshot for all nodes in the graph
		// (used by BuildStateResponse in "changed" mode to compute the diff)
		Data->PreOpPinConnections.Empty();
		if (UEdGraph* SnapGraph = Data->Graph.Get())
		{
			for (UEdGraphNode* SnapNode : SnapGraph->Nodes)
			{
				if (!SnapNode)
				{
					continue;
				}
				TMap<FName, TArray<FString>> PinConns;
				for (UEdGraphPin* SnapPin : SnapNode->Pins)
				{
					if (!SnapPin)
					{
						continue;
					}
					TArray<FString> ConnectedTo;
					for (UEdGraphPin* LinkedPin : SnapPin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode())
						{
							ConnectedTo.Add(LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
						}
					}
					PinConns.Add(SnapPin->PinName, ConnectedTo);
				}
				Data->PreOpPinConnections.Add(SnapNode->NodeGuid, PinConns);
			}
		}

		// Helper lambda to emit debug warning if a mutation op left affected nodes empty
		auto CheckMutationAffectedNodes = [&](const FString& OpName, const FToolResult& Result)
		{
			if (!Result.bIsError && Data->ResponseMode == TEXT("changed") && Data->LastOperationAffectedNodes.IsEmpty())
			{
				UE_LOG(LogClaireon, Warning,
					TEXT("response_mode=changed: no affected nodes recorded after mutation op '%s' — check handler"),
					*OpName);
			}
			return Result;
		};

		// Route to specific operation
		if (Operation == TEXT("add_node"))
		{
			return CheckMutationAffectedNodes(Operation, Operation_AddNode(SessionId, Data, Params));
		}
		else if (Operation == TEXT("connect_pins"))
		{
			return CheckMutationAffectedNodes(Operation, Operation_ConnectPins(SessionId, Data, Params));
		}
		else if (Operation == TEXT("disconnect_pin"))
		{
			return CheckMutationAffectedNodes(Operation, Operation_DisconnectPin(SessionId, Data, Params));
		}
		else if (Operation == TEXT("set_pin_value"))
		{
			return CheckMutationAffectedNodes(Operation, Operation_SetPinValue(SessionId, Data, Params));
		}
		else if (Operation == TEXT("add_pin"))
		{
			return CheckMutationAffectedNodes(Operation, Operation_AddPin(SessionId, Data, Params));
		}
		else if (Operation == TEXT("remove_pin"))
		{
			return CheckMutationAffectedNodes(Operation, Operation_RemovePin(SessionId, Data, Params));
		}
		else if (Operation == TEXT("split_pin"))
		{
			return CheckMutationAffectedNodes(Operation, Operation_SplitPin(SessionId, Data, Params));
		}
		else if (Operation == TEXT("recombine_pin"))
		{
			return CheckMutationAffectedNodes(Operation, Operation_RecombinePin(SessionId, Data, Params));
		}
		else if (Operation == TEXT("add_variable"))
		{
			return Operation_AddVariable(SessionId, Data, Params);
		}
		else if (Operation == TEXT("set_variable_properties"))
		{
			return Operation_SetVariableProperties(SessionId, Data, Params);
		}
		else if (Operation == TEXT("add_component"))
		{
			return Operation_AddComponent(SessionId, Data, Params);
		}
		else if (Operation == TEXT("set_property"))
		{
			return Operation_SetProperty(SessionId, Data, Params);
		}
		else if (Operation == TEXT("remove_component"))
		{
			return Operation_RemoveComponent(SessionId, Data, Params);
		}
		else if (Operation == TEXT("reparent_component"))
		{
			return Operation_ReparentComponent(SessionId, Data, Params);
		}
		else if (Operation == TEXT("rename_component"))
		{
			return Operation_RenameComponent(SessionId, Data, Params);
		}
		else if (Operation == TEXT("set_root_component"))
		{
			return Operation_SetRootComponent(SessionId, Data, Params);
		}
		else if (Operation == TEXT("get_component_details"))
		{
			return Operation_GetComponentDetails(SessionId, Data, Params);
		}
		else if (Operation == TEXT("move_cursor"))
		{
			return Operation_MoveCursor(SessionId, Data, Params);
		}
		else if (Operation == TEXT("cursor_back"))
		{
			return Operation_CursorBack(SessionId, Data, Params);
		}
		else if (Operation == TEXT("select_node"))
		{
			return Operation_SelectNode(SessionId, Data, Params);
		}
		else if (Operation == TEXT("select_pin"))
		{
			return Operation_SelectPin(SessionId, Data, Params);
		}
		else if (Operation == TEXT("select_nearest_node"))
		{
			return Operation_SelectNearestNode(SessionId, Data, Params);
		}
		else if (Operation == TEXT("get_state"))
		{
			return Operation_GetState(SessionId, Data, Params);
		}
		else if (Operation == TEXT("import_nodes"))
		{
			return Operation_ImportNodes(SessionId, Data, Params);
		}
		else if (Operation == TEXT("compile"))
		{
			return Operation_Compile(SessionId, Data, Params);
		}
		else if (Operation == TEXT("save"))
		{
			return Operation_Save(SessionId, Data, Params);
		}
		else if (Operation == TEXT("format"))
		{
			return Operation_Format(SessionId, Data, Params);
		}
		else if (Operation == TEXT("move_node"))
		{
			return CheckMutationAffectedNodes(Operation, Operation_MoveNode(SessionId, Data, Params));
		}
		else if (Operation == TEXT("add_function_override"))
		{
			return CheckMutationAffectedNodes(Operation, Operation_AddFunctionOverride(SessionId, Data, Params));
		}
		else if (Operation == TEXT("close"))
		{
			return Operation_Close(SessionId, Data, Params);
		}
		else
		{
			return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
		}
	}
}

// ============================================================================
// Session Delegate
// ============================================================================

void ClaireonTool_EditBlueprintGraph::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("editor.blueprint.edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

// ============================================================================
// GUID lookup helper — wraps FindNodeByGuid and records A-field fallback
// corrections so they can be surfaced in the MCP response.
// ============================================================================

static UEdGraphNode* FindNodeForOperation(
	UEdGraph* Graph,
	const FGuid& RequestedGuid,
	FBlueprintEditToolData* Data)
{
	FGuid CorrectedGuid;
	UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, RequestedGuid, &CorrectedGuid);
	if (Node && CorrectedGuid.IsValid() && Data)
	{
		Data->GuidCorrections.Add(RequestedGuid, CorrectedGuid);
	}
	return Node;
}

// ============================================================================
// Operations (Placeholders - will be implemented incrementally)
// ============================================================================

FToolResult ClaireonTool_EditBlueprintGraph::Operation_Open(const TSharedPtr<FJsonObject>& Params)
{
	// Register delegate on first use
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_EditBlueprintGraph::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	// Get asset_path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	// Load Blueprint
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));
	}

	// Get graph_name (optional, defaults to EventGraph)
	FString GraphName;
	if (!Params->TryGetStringField(TEXT("graph_name"), GraphName))
	{
		GraphName = TEXT("EventGraph");
	}

	// Find the graph
	UEdGraph* Graph = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, GraphName);
	if (!Graph)
	{
		return MakeErrorResult(FString::Printf(TEXT("Graph '%s' not found in Blueprint %s"), *GraphName, *AssetPath));
	}

	// Open session via the manager (handles locking)
	double TimeoutMinutes = 60.0;
	Params->TryGetNumberField(TEXT("timeout_minutes"), TimeoutMinutes);
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(AssetPath, TEXT("editor.blueprint.edit"), TimeoutMinutes);

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first, or use mcp_release_sessions(asset_path='%s') to force-release it."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60,
			*AssetPath));
	}

	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *AssetPath));
	}

	const FString& SessionId = OpenResult.SessionId;

	// Create tool-specific data (or reuse if session was reused)
	FBlueprintEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		FBlueprintEditToolData NewData;
		NewData.Blueprint = Blueprint;
		NewData.Graph = Graph;
		NewData.Cursor.GraphName = Graph->GetName();
		NewData.Cursor.ViewportCenter = FVector2D(0.0f, 0.0f);

		// Find first event node to focus cursor
		TArray<UEdGraphNode*> RootNodes = ClaireonBlueprintHelpers::FindRootNodes(Graph);
		if (RootNodes.Num() > 0)
		{
			UEdGraphNode* FirstNode = RootNodes[0];
			NewData.Cursor.FocusedNodeGuid = FirstNode->NodeGuid;
			UEdGraphPin* FirstOutput = ClaireonBlueprintHelpers::GetFirstOutputPin(FirstNode);
			if (FirstOutput)
			{
				NewData.Cursor.FocusedPinName = FirstOutput->PinName;
				NewData.Cursor.FocusedPinDirection = FirstOutput->Direction;
			}
		}

		ToolData.Add(SessionId, MoveTemp(NewData));
		Data = ToolData.Find(SessionId);

		UE_LOG(LogClaireon, Log, TEXT("[EditBlueprintGraph] Created session %s for Blueprint %s"), *SessionId, *Blueprint->GetPathName());
	}

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Opened Blueprint %s, Graph %s"), *AssetPath, *GraphName);

	// "open" always returns the full graph regardless of response_mode — gives initial orientation
	Data->ResponseMode = TEXT("full");
	Data->bSuppressOutput = false;

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_Create(const TSharedPtr<FJsonObject>& Params)
{
	// Get asset_path
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	// Validate asset path
	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	// Get parent_class (optional, defaults to Actor)
	FString ParentClassName;
	if (!Params->TryGetStringField(TEXT("parent_class"), ParentClassName))
	{
		ParentClassName = TEXT("Actor");
	}

	// Find parent class
	ClaireonNameResolver::FNameResolveResult ParentClassResult;
	UClass* ParentClass = ClaireonNameResolver::ResolveClassName(ParentClassName, nullptr, ParentClassResult);
	if (!ParentClass)
	{
		return MakeErrorResult(ParentClassResult.Error);
	}
	TArray<FString> ResolutionWarnings;
	if (!ParentClassResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(ParentClassResult.ResolutionNote);
	}

	// Extract package and asset name from path
	FString PackageName = AssetPath;
	FString AssetName;
	if (AssetPath.Contains(TEXT(".")))
	{
		AssetPath.Split(TEXT("."), &PackageName, &AssetName);
	}
	else
	{
		// Asset name from last path component
		int32 LastSlash;
		if (PackageName.FindLastChar('/', LastSlash))
		{
			AssetName = PackageName.Mid(LastSlash + 1);
		}
		else
		{
			AssetName = TEXT("NewBlueprint");
		}
	}

	// Check if package already exists on disk (e.g., from previous test run)
	// If it does, we must delete it first to avoid "partially loaded" errors
	FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (FPaths::FileExists(PackageFileName))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[EditBlueprintGraph] Create: Deleting existing file %s"), *PackageFileName);
		IFileManager::Get().Delete(*PackageFileName, false, true);
	}

	// Create package
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
	}

	// Create Blueprint with proper flags matching editor workflow
	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None);

	if (!Blueprint)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create Blueprint at %s"), *AssetPath));
	}

	// Configure package to match editor workflow (see UBlueprintFactory and FAssetToolsImpl::CreateAsset)
	Package->SetIsExternallyReferenceable(true); // Mark as externally referenceable asset
	Package->MarkPackageDirty();

	// Notify asset registry
	FAssetRegistryModule::AssetCreated(Blueprint);

	// Get EventGraph (created by default)
	UEdGraph* EventGraph = nullptr;
	if (Blueprint->UbergraphPages.Num() > 0)
	{
		EventGraph = Blueprint->UbergraphPages[0];
	}

	if (!EventGraph)
	{
		return MakeErrorResult(TEXT("Failed to find EventGraph in newly created Blueprint"));
	}

	// Register delegate on first use
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonTool_EditBlueprintGraph::HandleSessionClosed);
		bDelegateRegistered = true;
	}

	// Open session via the manager (handles locking)
	double TimeoutMinutes = 60.0;
	Params->TryGetNumberField(TEXT("timeout_minutes"), TimeoutMinutes);
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(Blueprint->GetPathName(), TEXT("editor.blueprint.edit"), TimeoutMinutes);

	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first, or use mcp_release_sessions(asset_path='%s') to force-release it."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60,
			*Blueprint->GetPathName()));
	}

	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path for created Blueprint: %s"), *Blueprint->GetPathName()));
	}

	const FString& SessionId = OpenResult.SessionId;

	// Create tool-specific data
	FBlueprintEditToolData NewData;
	NewData.Blueprint = Blueprint;
	NewData.Graph = EventGraph;
	NewData.Cursor.GraphName = EventGraph->GetName();
	NewData.Cursor.ViewportCenter = FVector2D(0.0f, 0.0f);

	// Find first event node to focus cursor
	TArray<UEdGraphNode*> RootNodes = ClaireonBlueprintHelpers::FindRootNodes(EventGraph);
	if (RootNodes.Num() > 0)
	{
		UEdGraphNode* FirstNode = RootNodes[0];
		NewData.Cursor.FocusedNodeGuid = FirstNode->NodeGuid;
		UEdGraphPin* FirstOutput = ClaireonBlueprintHelpers::GetFirstOutputPin(FirstNode);
		if (FirstOutput)
		{
			NewData.Cursor.FocusedPinName = FirstOutput->PinName;
			NewData.Cursor.FocusedPinDirection = FirstOutput->Direction;
		}
	}

	ToolData.Add(SessionId, MoveTemp(NewData));
	FBlueprintEditToolData* Data = ToolData.Find(SessionId);

	UE_LOG(LogClaireon, Log, TEXT("[EditBlueprintGraph] Created session %s for new Blueprint %s"), *SessionId, *Blueprint->GetPathName());

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Created new Blueprint %s with parent class %s"), *AssetPath, *ParentClassName);

	FToolResult CreateResult = BuildStateResponse(SessionId, Data);
	CreateResult.Warnings.Append(ResolutionWarnings);
	return CreateResult;
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_AddNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

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

		UK2Node_CallFunction* CallFuncNode = NewObject<UK2Node_CallFunction>(Graph);

		if (!FunctionClass.IsEmpty())
		{
			ClaireonNameResolver::FNameResolveResult FuncClassResult;
			UClass* OwnerClass = ClaireonNameResolver::ResolveClassName(FunctionClass, nullptr, FuncClassResult);
			if (OwnerClass)
			{
				CallFuncNode->FunctionReference.SetExternalMember(FName(*FunctionName), OwnerClass);
				if (!FuncClassResult.ResolutionNote.IsEmpty())
				{
					ResolutionWarnings.Add(FuncClassResult.ResolutionNote);
				}
			}
			else
			{
				CallFuncNode->FunctionReference.SetSelfMember(FName(*FunctionName));
			}
		}
		else
		{
			CallFuncNode->FunctionReference.SetSelfMember(FName(*FunctionName));
		}

		NewNode = CallFuncNode;
		NodeDescription = FString::Printf(TEXT("CallFunction: %s"), *FunctionName);
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
	// --- Macro nodes (StandardMacros library aliases + generic Macro type) ---
	else if (NodeType == TEXT("Macro") || NodeType == TEXT("ForEachLoop") || NodeType == TEXT("ForEachLoopWithBreak")
		|| NodeType == TEXT("ForLoop") || NodeType == TEXT("ForLoopWithBreak") || NodeType == TEXT("WhileLoop")
		|| NodeType == TEXT("DoOnce") || NodeType == TEXT("DoN") || NodeType == TEXT("FlipFlop")
		|| NodeType == TEXT("Gate") || NodeType == TEXT("MultiGate") || NodeType == TEXT("IsValid"))
	{
		// Determine macro name and library path
		FString MacroName;
		FString MacroLibraryPath = TEXT("/Engine/EditorBlueprintResources/StandardMacros");

		if (NodeType == TEXT("Macro"))
		{
			if (!Params->TryGetStringField(TEXT("macro_name"), MacroName))
			{
				return MakeErrorResult(TEXT("Missing required field 'macro_name' for Macro node type"));
			}
			FString CustomLibPath;
			if (Params->TryGetStringField(TEXT("macro_library_path"), CustomLibPath))
			{
				MacroLibraryPath = CustomLibPath;
			}
		}
		else
		{
			// Named alias — NodeType IS the macro name
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
				TEXT("Failed to create timeline '%s' — Blueprint may not support timelines"),
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
	Data->Cursor.PushHistory();
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

FToolResult ClaireonTool_EditBlueprintGraph::Operation_RemoveNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	// Get node_guid
	FString NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));
	}

	// Find the node
	UEdGraphNode* Node = FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

	// Capture exec-connected neighbors BEFORE removing (they change after BreakAllPinLinks)
	for (UEdGraphPin* RemPin : Node->Pins)
	{
		if (RemPin && RemPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			for (UEdGraphPin* LinkedRemPin : RemPin->LinkedTo)
			{
				if (LinkedRemPin && LinkedRemPin->GetOwningNode())
				{
					Data->LastOperationAffectedNodes.Add(LinkedRemPin->GetOwningNode()->NodeGuid);
				}
			}
		}
	}

	// Remove the node using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Blueprint Node")));
	Blueprint->Modify();
	Graph->Modify();

	// Break all pin connections first
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			Pin->BreakAllPinLinks();
		}
	}

	// Remove from graph
	Graph->RemoveNode(Node);

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	// Update cursor if it was pointing to removed node
	if (Data->Cursor.FocusedNodeGuid == NodeGuid)
	{
		ValidateCursor(Data);
	}

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Removed node: %s"), *NodeTitle);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_ConnectPins(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	// Get source pin name (required)
	FString SourcePinName;
	if (!Params->TryGetStringField(TEXT("source_pin_name"), SourcePinName))
	{
		return MakeErrorResult(TEXT("Missing required field: source_pin_name"));
	}

	// Get target pin name (required)
	FString TargetPinName;
	if (!Params->TryGetStringField(TEXT("target_pin_name"), TargetPinName))
	{
		return MakeErrorResult(TEXT("Missing required field: target_pin_name"));
	}

	// Find source node (by GUID or title)
	UEdGraphNode* SourceNode = nullptr;
	FString SourceNodeGuidStr, SourceNodeTitle;

	if (Params->TryGetStringField(TEXT("source_node_guid"), SourceNodeGuidStr))
	{
		// Find by GUID
		FGuid SourceNodeGuid;
		if (!FGuid::Parse(SourceNodeGuidStr, SourceNodeGuid))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid source_node_guid format: %s"), *SourceNodeGuidStr));
		}
		SourceNode = FindNodeForOperation(Graph, SourceNodeGuid, Data);
		if (!SourceNode)
		{
			FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
			return MakeErrorResult(FString::Printf(TEXT("Source node not found with GUID: %s in graph '%s'.\n%s"),
				*SourceNodeGuidStr, *Graph->GetName(), *AvailableNodes));
		}
	}
	else if (Params->TryGetStringField(TEXT("source_node_title"), SourceNodeTitle))
	{
		// Find by title
		TArray<UEdGraphNode*> MatchingNodes = ClaireonBlueprintHelpers::FindNodesByTitle(Graph, SourceNodeTitle, true);
		if (MatchingNodes.Num() == 0)
		{
			return MakeErrorResult(FString::Printf(TEXT("Source node not found with title: %s"), *SourceNodeTitle));
		}
		else if (MatchingNodes.Num() > 1)
		{
			return MakeErrorResult(FString::Printf(TEXT("Ambiguous source node title '%s' - %d nodes match. Use source_node_guid instead."), *SourceNodeTitle, MatchingNodes.Num()));
		}
		SourceNode = MatchingNodes[0];
	}
	else
	{
		return MakeErrorResult(TEXT("Missing required field: source_node_guid or source_node_title"));
	}

	// Find target node (by GUID or title)
	UEdGraphNode* TargetNode = nullptr;
	FString TargetNodeGuidStr, TargetNodeTitle;

	if (Params->TryGetStringField(TEXT("target_node_guid"), TargetNodeGuidStr))
	{
		// Find by GUID
		FGuid TargetNodeGuid;
		if (!FGuid::Parse(TargetNodeGuidStr, TargetNodeGuid))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid target_node_guid format: %s"), *TargetNodeGuidStr));
		}
		TargetNode = FindNodeForOperation(Graph, TargetNodeGuid, Data);
		if (!TargetNode)
		{
			FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
			return MakeErrorResult(FString::Printf(TEXT("Target node not found with GUID: %s in graph '%s'.\n%s"),
				*TargetNodeGuidStr, *Graph->GetName(), *AvailableNodes));
		}
	}
	else if (Params->TryGetStringField(TEXT("target_node_title"), TargetNodeTitle))
	{
		// Find by title
		TArray<UEdGraphNode*> MatchingNodes = ClaireonBlueprintHelpers::FindNodesByTitle(Graph, TargetNodeTitle, true);
		if (MatchingNodes.Num() == 0)
		{
			return MakeErrorResult(FString::Printf(TEXT("Target node not found with title: %s"), *TargetNodeTitle));
		}
		else if (MatchingNodes.Num() > 1)
		{
			return MakeErrorResult(FString::Printf(TEXT("Ambiguous target node title '%s' - %d nodes match. Use target_node_guid instead."), *TargetNodeTitle, MatchingNodes.Num()));
		}
		TargetNode = MatchingNodes[0];
	}
	else
	{
		return MakeErrorResult(TEXT("Missing required field: target_node_guid or target_node_title"));
	}

	// Resolve source pin using fuzzy matching
	TArray<FString> ResolutionWarnings;
	EEdGraphPinDirection SourceDirHint = EGPD_Output;
	FString SourcePinDirection;
	if (Params->TryGetStringField(TEXT("source_pin_direction"), SourcePinDirection))
	{
		SourceDirHint = (SourcePinDirection == TEXT("input")) ? EGPD_Input : EGPD_Output;
	}
	ClaireonNameResolver::FNameResolveResult SourcePinResult;
	UEdGraphPin* SourcePin = ClaireonNameResolver::ResolvePinName(SourceNode, SourcePinName, SourceDirHint, SourcePinResult);
	if (!SourcePin)
	{
		return MakeErrorResult(SourcePinResult.Error);
	}
	if (!SourcePinResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(SourcePinResult.ResolutionNote);
	}

	// Resolve target pin using fuzzy matching
	EEdGraphPinDirection TargetDirHint = EGPD_Input;
	FString TargetPinDirection;
	if (Params->TryGetStringField(TEXT("target_pin_direction"), TargetPinDirection))
	{
		TargetDirHint = (TargetPinDirection == TEXT("input")) ? EGPD_Input : EGPD_Output;
	}
	ClaireonNameResolver::FNameResolveResult TargetPinResult;
	UEdGraphPin* TargetPin = ClaireonNameResolver::ResolvePinName(TargetNode, TargetPinName, TargetDirHint, TargetPinResult);
	if (!TargetPin)
	{
		return MakeErrorResult(TargetPinResult.Error);
	}
	if (!TargetPinResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(TargetPinResult.ResolutionNote);
	}

	// Validate connection compatibility
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	FPinConnectionResponse Response = K2Schema->CanCreateConnection(SourcePin, TargetPin);

	if (Response.Response == CONNECT_RESPONSE_DISALLOW)
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot connect pins: %s"), *Response.Message.ToString()));
	}

	// Make the connection using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Connect Blueprint Pins")));
	Blueprint->Modify();
	Graph->Modify();

	// Break existing connections if needed
	if (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A)
	{
		SourcePin->BreakAllPinLinks();
	}
	if (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B)
	{
		TargetPin->BreakAllPinLinks();
	}
	if (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB)
	{
		SourcePin->BreakAllPinLinks();
		TargetPin->BreakAllPinLinks();
	}

	// Make the connection
	SourcePin->MakeLinkTo(TargetPin);

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	const FString SourceTitle = SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
	const FString TargetTitle = TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString();

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Connected: [%s].%s -> [%s].%s"),
		*SourceTitle, *SourcePinName,
		*TargetTitle, *TargetPinName);

	// Populate affected nodes: both endpoint nodes
	Data->LastOperationAffectedNodes.Add(SourceNode->NodeGuid);
	Data->LastOperationAffectedNodes.Add(TargetNode->NodeGuid);

	FToolResult ConnectResult = BuildStateResponse(SessionId, Data);
	ConnectResult.Warnings.Append(ResolutionWarnings);
	return ConnectResult;
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_DisconnectPin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	// Get node and pin
	FString NodeGuidStr, PinName;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}
	if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
	{
		return MakeErrorResult(TEXT("Missing required field: pin_name"));
	}

	// Parse GUID
	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));
	}

	// Find node
	UEdGraphNode* Node = FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	// Resolve pin using fuzzy matching
	TArray<FString> ResolutionWarnings;
	ClaireonNameResolver::FNameResolveResult DisconnectPinResult;
	UEdGraphPin* Pin = ClaireonNameResolver::ResolvePinName(Node, PinName, EGPD_MAX, DisconnectPinResult);
	if (!Pin)
	{
		return MakeErrorResult(DisconnectPinResult.Error);
	}
	if (!DisconnectPinResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(DisconnectPinResult.ResolutionNote);
	}

	int32 ConnectionCount = Pin->LinkedTo.Num();
	if (ConnectionCount == 0)
	{
		return MakeErrorResult(FString::Printf(TEXT("Pin '%s' has no connections to break"), *PinName));
	}

	// Capture both endpoint nodes BEFORE breaking (links gone after BreakAllPinLinks)
	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
	for (UEdGraphPin* LinkedDisconPin : Pin->LinkedTo)
	{
		if (LinkedDisconPin && LinkedDisconPin->GetOwningNode())
		{
			Data->LastOperationAffectedNodes.Add(LinkedDisconPin->GetOwningNode()->NodeGuid);
		}
	}

	// Break connections using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Disconnect Blueprint Pin")));
	Blueprint->Modify();
	Graph->Modify();

	Pin->BreakAllPinLinks();

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Disconnected %d connection(s) from [%s].%s"),
		ConnectionCount, *NodeTitle, *PinName);

	FToolResult DisconnectResult = BuildStateResponse(SessionId, Data);
	DisconnectResult.Warnings.Append(ResolutionWarnings);
	return DisconnectResult;
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_SetPinValue(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	// Get node and pin
	FString NodeGuidStr, PinName, Value;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}
	if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
	{
		return MakeErrorResult(TEXT("Missing required field: pin_name"));
	}
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required field: value"));
	}

	// Parse GUID
	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));
	}

	// Find node
	UEdGraphNode* Node = FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	// Resolve pin using fuzzy matching
	TArray<FString> ResolutionWarnings;
	ClaireonNameResolver::FNameResolveResult SetPinResult;
	UEdGraphPin* Pin = ClaireonNameResolver::ResolvePinName(Node, PinName, EGPD_MAX, SetPinResult);
	if (!Pin)
	{
		return MakeErrorResult(SetPinResult.Error);
	}
	if (!SetPinResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(SetPinResult.ResolutionNote);
	}

	// Validate pin can have default value (must be input pin with no connection)
	if (Pin->Direction != EGPD_Input)
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot set default value on output pin: %s"), *PinName));
	}

	if (Pin->LinkedTo.Num() > 0)
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot set default value on connected pin: %s"), *PinName));
	}

	// Set the default value using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blueprint Pin Value")));
	Blueprint->Modify();
	Graph->Modify();
	Node->Modify();

	// Get schema for validation
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
	K2Schema->TrySetDefaultValue(*Pin, Value);

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Set [%s].%s = '%s'"),
		*NodeTitle, *PinName, *Value);

	// Populate affected nodes: the node whose pin value changed
	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);

	FToolResult SetPinValueResult = BuildStateResponse(SessionId, Data);
	SetPinValueResult.Warnings.Append(ResolutionWarnings);
	return SetPinValueResult;
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_AddVariable(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();

	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// Get variable name and type
	FString VarName, VarType;
	if (!Params->TryGetStringField(TEXT("variable_name"), VarName))
	{
		return MakeErrorResult(TEXT("Missing required field: variable_name"));
	}
	if (!Params->TryGetStringField(TEXT("variable_type"), VarType))
	{
		return MakeErrorResult(TEXT("Missing required field: variable_type"));
	}

	// Get optional default value
	FString DefaultValue;
	Params->TryGetStringField(TEXT("default_value"), DefaultValue);

	// Parse variable type
	FEdGraphPinType PinType = ClaireonBlueprintHelpers::ParseVariableType(VarType);

	// Check if variable already exists
	for (const FBPVariableDescription& ExistingVar : Blueprint->NewVariables)
	{
		if (ExistingVar.VarName == FName(*VarName))
		{
			return MakeErrorResult(FString::Printf(TEXT("Variable '%s' already exists"), *VarName));
		}
	}

	// Create variable using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Blueprint Variable")));
	Blueprint->Modify();

	// Create new variable
	FBPVariableDescription NewVar;
	NewVar.VarName = FName(*VarName);
	NewVar.VarType = PinType;
	NewVar.FriendlyName = VarName;
	NewVar.Category = FText::FromString(TEXT("Default"));

	// Set default value if provided
	if (!DefaultValue.IsEmpty())
	{
		NewVar.DefaultValue = DefaultValue;
	}

	// Add to Blueprint
	int32 VarIndex = Blueprint->NewVariables.Add(NewVar);

	// Apply optional properties (flags, category, replication, metadata, etc.)
	// Must be called after the variable is added to NewVariables since the
	// FBlueprintEditorUtils setter functions look up the variable by name.
	ClaireonBlueprintHelpers::ApplyVariableProperties(Blueprint, FName(*VarName), Params);

	// Mark Blueprint as structurally modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Added variable: %s (%s)"),
		*VarName, *VarType);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_SetVariableProperties(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();

	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// Get variable name
	FString VarName;
	if (!Params->TryGetStringField(TEXT("variable_name"), VarName))
	{
		return MakeErrorResult(TEXT("Missing required field: variable_name"));
	}

	// Find the variable
	FName VarFName(*VarName);
	bool bFound = false;
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarFName)
		{
			bFound = true;
			break;
		}
	}

	if (!bFound)
	{
		return MakeErrorResult(FString::Printf(TEXT("Variable '%s' not found in Blueprint"), *VarName));
	}

	// Apply properties within a transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Variable Properties")));
	Blueprint->Modify();

	ClaireonBlueprintHelpers::ApplyVariableProperties(Blueprint, VarFName, Params);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Set properties on variable: %s"),
		*VarName);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_AddComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();

	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// Get component name and class
	FString ComponentName, ComponentClass;
	if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return MakeErrorResult(TEXT("Missing required field: component_name"));
	}
	if (!Params->TryGetStringField(TEXT("component_class"), ComponentClass))
	{
		return MakeErrorResult(TEXT("Missing required field: component_class"));
	}

	// Find component class using fuzzy resolution (handles U prefix, Component suffix, etc.)
	TArray<FString> ResolutionWarnings;
	ClaireonNameResolver::FNameResolveResult CompClassResult;
	UClass* CompClass = ClaireonNameResolver::ResolveClassName(ComponentClass, UActorComponent::StaticClass(), CompClassResult);
	if (!CompClass)
	{
		return MakeErrorResult(CompClassResult.Error);
	}
	if (!CompClassResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(CompClassResult.ResolutionNote);
	}

	if (!CompClass->IsChildOf(UActorComponent::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Class '%s' is not a component class"), *ComponentClass));
	}

	// Get or create SimpleConstructionScript
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return MakeErrorResult(TEXT("Blueprint does not have a SimpleConstructionScript (not an Actor Blueprint?)"));
	}

	// Get optional parent component
	FString ParentComponentName;
	USCS_Node* ParentNode = nullptr;
	if (Params->TryGetStringField(TEXT("parent_component"), ParentComponentName))
	{
		// Find parent component node
		ParentNode = SCS->FindSCSNode(FName(*ParentComponentName));

		if (!ParentNode)
		{
			return MakeErrorResult(FString::Printf(TEXT("Parent component not found: %s"), *ParentComponentName));
		}
	}

	// Create component using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Blueprint Component")));
	Blueprint->Modify();
	SCS->Modify();

	// Create new SCS node
	USCS_Node* NewNode = SCS->CreateNode(CompClass, FName(*ComponentName));
	if (!NewNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create component node: %s"), *ComponentName));
	}

	// Add to SCS
	if (ParentNode)
	{
		ParentNode->AddChildNode(NewNode);
	}
	else
	{
		SCS->AddNode(NewNode);
	}

	// Mark Blueprint as structurally modified
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Added component: %s (%s)"),
		*ComponentName, *ComponentClass);

	FToolResult AddCompResult = BuildStateResponse(SessionId, Data);
	AddCompResult.Warnings.Append(ResolutionWarnings);
	return AddCompResult;
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_SetProperty(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();

	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// Get property name and value
	FString PropertyName, PropertyValue;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}
	if (!Params->TryGetStringField(TEXT("property_value"), PropertyValue))
	{
		return MakeErrorResult(TEXT("Missing required field: property_value"));
	}

	// Get optional component name (if not specified, set on CDO)
	FString ComponentName;
	UObject* TargetObject = nullptr;
	FString TargetDescription;

	if (Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		// Find component in SCS
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		if (!SCS)
		{
			return MakeErrorResult(TEXT("Blueprint does not have a SimpleConstructionScript"));
		}

		USCS_Node* ComponentNode = SCS->FindSCSNode(FName(*ComponentName));

		if (!ComponentNode)
		{
			return MakeErrorResult(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
		}

		TargetObject = ComponentNode->ComponentTemplate;
		TargetDescription = FString::Printf(TEXT("Component '%s'"), *ComponentName);
	}
	else
	{
		// Set property on CDO
		if (Blueprint->GeneratedClass)
		{
			TargetObject = Blueprint->GeneratedClass->GetDefaultObject();
			TargetDescription = TEXT("Blueprint CDO");
		}

		if (!TargetObject)
		{
			return MakeErrorResult(TEXT("Failed to get Blueprint CDO"));
		}
	}

	// Find property on target object
	FProperty* Property = TargetObject->GetClass()->FindPropertyByName(FName(*PropertyName));
	if (!Property)
	{
		return MakeErrorResult(FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *TargetDescription));
	}

	// Check if property is editable
	if (Property->HasAnyPropertyFlags(CPF_DisableEditOnInstance))
	{
		return MakeErrorResult(FString::Printf(TEXT("Property '%s' is not editable"), *PropertyName));
	}

	// Set property using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blueprint Property")));
	Blueprint->Modify();
	TargetObject->Modify();

	// Smart detection: GameplayTagContainer from JSON array
	FStructProperty* MaybeTagContainerProp = CastField<FStructProperty>(Property);
	if (MaybeTagContainerProp && MaybeTagContainerProp->Struct == TBaseStructure<FGameplayTagContainer>::Get() && PropertyValue.StartsWith(TEXT("[")))
	{
		// Parse JSON array of tag name strings
		TArray<TSharedPtr<FJsonValue>> TagArray;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PropertyValue);
		if (!FJsonSerializer::Deserialize(Reader, TagArray))
		{
			return MakeErrorResult(TEXT("property_value starts with '[' but is not valid JSON array"));
		}

		FGameplayTagContainer NewContainer;
		UGameplayTagsManager& TagsManager = UGameplayTagsManager::Get();
		TArray<FString> Warnings;

		for (const TSharedPtr<FJsonValue>& Val : TagArray)
		{
			FString TagName;
			if (!Val->TryGetString(TagName))
				continue;
			FGameplayTag Tag = TagsManager.RequestGameplayTag(FName(*TagName), false);
			if (Tag.IsValid())
				NewContainer.AddTag(Tag);
			else
				Warnings.Add(FString::Printf(TEXT("[WARN] Tag '%s' not registered — skipped"), *TagName));
		}

		FGameplayTagContainer* Target =
			MaybeTagContainerProp->ContainerPtrToValuePtr<FGameplayTagContainer>(TargetObject);
		*Target = NewContainer;

		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

		FString WarningText = Warnings.IsEmpty() ? TEXT("") : FString::Join(Warnings, TEXT("\n")) + TEXT("\n");
		Data->Cursor.LastOperationStatus = FString::Printf(
			TEXT("Set %s.%s = [%d tags]"), *TargetDescription, *PropertyName, NewContainer.Num());

		// Prepend warnings to the normal state response
		FToolResult StateResult = BuildStateResponse(SessionId, Data);
		if (!WarningText.IsEmpty())
		{
			StateResult.Summary = WarningText + StateResult.Summary;
		}
		return StateResult;
	}

	// Import property value from string
	const TCHAR* ValuePtr = *PropertyValue;
	Property->ImportText_Direct(ValuePtr, Property->ContainerPtrToValuePtr<void>(TargetObject), TargetObject, PPF_None);

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Set %s.%s = '%s'"),
		*TargetDescription, *PropertyName, *PropertyValue);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_RemoveComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return MakeErrorResult(TEXT("Blueprint does not have a SimpleConstructionScript (not an Actor Blueprint?)"));
	}

	// Parse component_name
	FString ComponentName;
	if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return MakeErrorResult(TEXT("Missing required field: component_name"));
	}

	// Find node
	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
	}

	// Verify not inherited (must be in SCS->GetAllNodes(), not only in parent BP's SCS)
	if (!SCS->GetAllNodes().Contains(Node))
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot remove inherited component: %s"), *ComponentName));
	}

	// Verify not DefaultSceneRootNode
	if (Node == SCS->GetDefaultSceneRootNode())
	{
		return MakeErrorResult(TEXT("Cannot remove the DefaultSceneRootNode directly. Add another scene component first, or use set_root_component."));
	}

	// Perform removal with undo support
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Blueprint Component")));
	SCS->Modify();

	// RemoveNodeAndPromoteChildren handles:
	// - If root node: promotes first non-editor-only child to root, moves remaining children under it, calls ValidateSceneRootNodes()
	// - If non-root node: moves children to the removed node's parent at the same position
	SCS->RemoveNodeAndPromoteChildren(Node);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Removed component: %s (children promoted)"), *ComponentName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_ReparentComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return MakeErrorResult(TEXT("Blueprint does not have a SimpleConstructionScript (not an Actor Blueprint?)"));
	}

	// Parse component_name (required)
	FString ComponentName;
	if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return MakeErrorResult(TEXT("Missing required field: component_name"));
	}

	// Find source node
	USCS_Node* SourceNode = SCS->FindSCSNode(FName(*ComponentName));
	if (!SourceNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
	}

	// Verify source not inherited
	if (!SCS->GetAllNodes().Contains(SourceNode))
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot reparent inherited component: %s"), *ComponentName));
	}

	// Parse optional parent_component
	FString ParentComponentName;
	bool bMoveToRoot = !Params->TryGetStringField(TEXT("parent_component"), ParentComponentName);
	USCS_Node* TargetNode = nullptr;

	if (!bMoveToRoot)
	{
		TargetNode = SCS->FindSCSNode(FName(*ParentComponentName));
		if (!TargetNode)
		{
			return MakeErrorResult(FString::Printf(TEXT("Target parent component not found: %s"), *ParentComponentName));
		}

		// Circular reparenting check: target must not be self or a descendant of source
		if (TargetNode == SourceNode)
		{
			return MakeErrorResult(TEXT("Cannot reparent a component to itself"));
		}
		// IsChildOf checks if TargetNode is a descendant of SourceNode
		// USCS_Node::IsChildOf(USCS_Node* TestParent) returns true if 'this' is a child of TestParent
		// So we check: is TargetNode a child of SourceNode? -> TargetNode->IsChildOf(SourceNode)
		if (TargetNode->IsChildOf(SourceNode))
		{
			return MakeErrorResult(FString::Printf(TEXT("Cannot reparent '%s' under '%s': would create a circular hierarchy (target is a descendant of source)"), *ComponentName, *ParentComponentName));
		}

		// Non-scene component cannot be attached to a parent
		if (SourceNode->ComponentTemplate && !SourceNode->ComponentTemplate->IsA<USceneComponent>())
		{
			return MakeErrorResult(FString::Printf(TEXT("Cannot attach non-scene component '%s' to a parent. Non-scene components must remain at root level."), *ComponentName));
		}
	}

	// Perform reparent with undo support
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Reparent Blueprint Component")));
	SCS->Modify();

	// Detach from current position
	USCS_Node* CurrentParent = SCS->FindParentNode(SourceNode);
	if (CurrentParent)
	{
		CurrentParent->RemoveChildNode(SourceNode);
	}
	else
	{
		// Source is a root node
		SCS->RemoveNode(SourceNode, /*bValidateSceneRootNodes=*/false);
	}

	// Attach to new position
	if (!bMoveToRoot)
	{
		TargetNode->AddChildNode(SourceNode);
		SourceNode->SetParent(TargetNode);
	}
	else
	{
		// Moving to root level
		// NOTE (v1): When reparenting to root in a Blueprint inheriting from another BP with a root
		// component, RemoveNode above clears ParentComponentOrVariableName. This is intentional for v1
		// and may need attention for inherited-BP scenarios.
		SCS->AddNode(SourceNode);
	}

	SCS->ValidateSceneRootNodes();
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	FString StatusMsg = bMoveToRoot
		? FString::Printf(TEXT("Reparented component '%s' to root level"), *ComponentName)
		: FString::Printf(TEXT("Reparented component '%s' under '%s'"), *ComponentName, *ParentComponentName);
	Data->Cursor.LastOperationStatus = StatusMsg;
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_RenameComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return MakeErrorResult(TEXT("Blueprint does not have a SimpleConstructionScript (not an Actor Blueprint?)"));
	}

	// Parse component_name and new_name
	FString ComponentName;
	if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return MakeErrorResult(TEXT("Missing required field: component_name"));
	}

	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return MakeErrorResult(TEXT("Missing required field: new_name"));
	}

	// Find node
	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
	}

	// Verify not inherited
	if (!SCS->GetAllNodes().Contains(Node))
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot rename inherited component: %s"), *ComponentName));
	}

	// Validate new name is a valid C++ identifier
	if (NewName.Len() == 0)
	{
		return MakeErrorResult(TEXT("New name cannot be empty"));
	}
	if (FChar::IsDigit(NewName[0]))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid name '%s': cannot start with a digit"), *NewName));
	}
	for (TCHAR Ch : NewName)
	{
		if (!FChar::IsAlnum(Ch) && Ch != TEXT('_'))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid name '%s': contains invalid character '%c'. Only alphanumeric characters and underscores are allowed."), *NewName, Ch));
		}
	}

	// Pre-check for conflicts in all scopes
	FName NewFName(*NewName);

	// Scope 1: SCS component variables
	for (USCS_Node* ExistingNode : SCS->GetAllNodes())
	{
		if (ExistingNode && ExistingNode != Node && ExistingNode->GetVariableName() == NewFName)
		{
			return MakeErrorResult(FString::Printf(TEXT("Name '%s' conflicts with existing SCS component variable"), *NewName));
		}
	}

	// Scope 2: Blueprint-level variables
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == NewFName)
		{
			return MakeErrorResult(FString::Printf(TEXT("Name '%s' conflicts with existing Blueprint variable"), *NewName));
		}
	}

	// Scope 3: Function graph names
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (Graph && Graph->GetFName() == NewFName)
		{
			return MakeErrorResult(FString::Printf(TEXT("Name '%s' conflicts with existing function name"), *NewName));
		}
	}

	// Perform rename
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Rename Blueprint Component")));

	// RenameComponentMemberVariable handles:
	// - Variable name update on the SCS node
	// - Reference replacement in Blueprint graphs via ReplaceVariableReferences
	// - Child Blueprint validation via ValidateBlueprintChildVariables
	// - Inheritable component handler refresh
	// - Structural modification marking
	FBlueprintEditorUtils::RenameComponentMemberVariable(Blueprint, Node, NewFName);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Renamed component '%s' to '%s'"), *ComponentName, *NewName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_SetRootComponent(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return MakeErrorResult(TEXT("Blueprint does not have a SimpleConstructionScript (not an Actor Blueprint?)"));
	}

	// Parse component_name
	FString ComponentName;
	if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return MakeErrorResult(TEXT("Missing required field: component_name"));
	}

	// Find target node
	USCS_Node* TargetNode = SCS->FindSCSNode(FName(*ComponentName));
	if (!TargetNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
	}

	// Verify not inherited
	if (!SCS->GetAllNodes().Contains(TargetNode))
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot set inherited component as root: %s"), *ComponentName));
	}

	// Verify target is a USceneComponent
	if (!TargetNode->ComponentTemplate || !TargetNode->ComponentTemplate->IsA<USceneComponent>())
	{
		return MakeErrorResult(FString::Printf(TEXT("Component '%s' is not a scene component and cannot be the root"), *ComponentName));
	}

	// Check if target is already the scene root
	USCS_Node* CurrentRootNode = nullptr;
	SCS->GetSceneRootComponentTemplate(false, &CurrentRootNode);
	if (CurrentRootNode == TargetNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Component '%s' is already the scene root"), *ComponentName));
	}

	// Get the current scene root (including DefaultSceneRootNode if applicable)
	USCS_Node* OldRootNode = nullptr;
	SCS->GetSceneRootComponentTemplate(true, &OldRootNode);
	bool bWasDefaultSceneRoot = (OldRootNode == SCS->GetDefaultSceneRootNode());

	// Perform root swap with undo support
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Root Component")));
	SCS->Modify();

	// Step 1: Detach target from current parent (if it has one)
	USCS_Node* TargetParent = SCS->FindParentNode(TargetNode);
	if (TargetParent)
	{
		TargetParent->RemoveChildNode(TargetNode);
	}

	// Reset target's relative transform since it becomes root
	if (USceneComponent* SceneComp = Cast<USceneComponent>(TargetNode->ComponentTemplate))
	{
		SceneComp->SetRelativeLocation(FVector::ZeroVector);
		SceneComp->SetRelativeRotation(FRotator::ZeroRotator);
		// Preserve scale intentionally -- scale is often set at authoring time
	}

	// Step 2: Remove old root from root node set
	// Suppress ValidateSceneRootNodes until we finish the swap.
	// The transient state (old root removed, new root not yet added) is safe because
	// ValidateSceneRootNodes will be called by AddNode in the next step, at which point
	// the new root is already in the root set. (Review item M1)
	if (OldRootNode)
	{
		SCS->RemoveNode(OldRootNode, /*bValidateSceneRootNodes=*/false);
	}

	// Step 3: Add target as new root
	// AddNode adds to RootNodes and calls ValidateSceneRootNodes, which will
	// auto-remove DefaultSceneRootNode since a real scene component now exists
	SCS->AddNode(TargetNode);

	// Step 4: Reparent old root under new root
	if (!bWasDefaultSceneRoot && OldRootNode)
	{
		// Old root was a real component -- make it a child of the new root
		TargetNode->AddChildNode(OldRootNode);
		OldRootNode->SetParent(TargetNode);
	}
	// If bWasDefaultSceneRoot, the old default root is cleaned up by ValidateSceneRootNodes
	// in step 3's AddNode call, so no reparenting is needed.

	// MarkBlueprintAsStructurallyModified must be the last call after all hierarchy manipulation (Review item L2)
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Set '%s' as the new root component"), *ComponentName);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_GetComponentDetails(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	if (!SCS)
	{
		return MakeErrorResult(TEXT("Blueprint does not have a SimpleConstructionScript (not an Actor Blueprint?)"));
	}

	// Parse component_name
	FString ComponentName;
	if (!Params->TryGetStringField(TEXT("component_name"), ComponentName))
	{
		return MakeErrorResult(TEXT("Missing required field: component_name"));
	}

	// Parse optional include_defaults (default false)
	bool bIncludeDefaults = false;
	Params->TryGetBoolField(TEXT("include_defaults"), bIncludeDefaults);

	// Find node
	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	if (!Node)
	{
		return MakeErrorResult(FString::Printf(TEXT("Component not found: %s"), *ComponentName));
	}

	UActorComponent* ComponentTemplate = Node->ComponentTemplate;
	if (!ComponentTemplate)
	{
		return MakeErrorResult(FString::Printf(TEXT("Component '%s' has no template object"), *ComponentName));
	}

	// Build component details JSON
	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
	Details->SetStringField(TEXT("class"), ComponentTemplate->GetClass()->GetName());

	// is_root: check against scene root
	USCS_Node* SceneRootNode = nullptr;
	SCS->GetSceneRootComponentTemplate(true, &SceneRootNode);
	Details->SetBoolField(TEXT("is_root"), Node == SceneRootNode);

	// parent
	USCS_Node* ParentNode = SCS->FindParentNode(Node);
	if (ParentNode)
	{
		Details->SetStringField(TEXT("parent"), ParentNode->GetVariableName().ToString());
	}
	// If no parent, omit the field (root-level component)

	// children
	TArray<TSharedPtr<FJsonValue>> ChildrenArray;
	for (USCS_Node* ChildNode : Node->GetChildNodes())
	{
		if (ChildNode)
		{
			ChildrenArray.Add(MakeShared<FJsonValueString>(ChildNode->GetVariableName().ToString()));
		}
	}
	Details->SetArrayField(TEXT("children"), ChildrenArray);

	// is_scene_component and transform
	bool bIsSceneComponent = ComponentTemplate->IsA<USceneComponent>();
	Details->SetBoolField(TEXT("is_scene_component"), bIsSceneComponent);

	if (bIsSceneComponent)
	{
		USceneComponent* SceneComp = Cast<USceneComponent>(ComponentTemplate);
		FVector Location = SceneComp->GetRelativeLocation();
		FRotator Rotation = SceneComp->GetRelativeRotation();
		FVector Scale = SceneComp->GetRelativeScale3D();

		Details->SetStringField(TEXT("relative_location"), FString::Printf(TEXT("X=%.2f Y=%.2f Z=%.2f"), Location.X, Location.Y, Location.Z));
		Details->SetStringField(TEXT("relative_rotation"), FString::Printf(TEXT("P=%.2f Y=%.2f R=%.2f"), Rotation.Pitch, Rotation.Yaw, Rotation.Roll));
		Details->SetStringField(TEXT("relative_scale"), FString::Printf(TEXT("X=%.2f Y=%.2f Z=%.2f"), Scale.X, Scale.Y, Scale.Z));
	}

	// Properties
	TArray<TSharedPtr<FJsonValue>> PropertiesArray;
	UObject* CDO = ComponentTemplate->GetClass()->GetDefaultObject();

	for (TFieldIterator<FProperty> PropIt(ComponentTemplate->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;

		// Only include editable properties
		if (!Property->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		// Skip deprecated and transient properties
		if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
		{
			continue;
		}

		// Get values as strings for comparison
		FString TemplateValue;
		Property->ExportTextItem_Direct(TemplateValue, Property->ContainerPtrToValuePtr<void>(ComponentTemplate), nullptr, ComponentTemplate, PPF_None);

		FString DefaultValue;
		Property->ExportTextItem_Direct(DefaultValue, Property->ContainerPtrToValuePtr<void>(CDO), nullptr, CDO, PPF_None);

		// If not including defaults, skip properties that match CDO
		if (!bIncludeDefaults && TemplateValue == DefaultValue)
		{
			continue;
		}

		TSharedPtr<FJsonObject> PropObj = MakeShared<FJsonObject>();
		PropObj->SetStringField(TEXT("name"), Property->GetName());
		PropObj->SetStringField(TEXT("type"), Property->GetCPPType());
		PropObj->SetStringField(TEXT("value"), TemplateValue);
		PropertiesArray.Add(MakeShared<FJsonValueObject>(PropObj));
	}
	Details->SetArrayField(TEXT("properties"), PropertiesArray);

	// Serialize the details JSON to a string and embed in BuildStateResponse
	FString DetailsString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&DetailsString);
	FJsonSerializer::Serialize(Details.ToSharedRef(), Writer);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Component details for '%s':\n%s"), *ComponentName, *DetailsString);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_MoveCursor(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UEdGraph* Graph = Data->Graph.Get();

	if (!Graph)
	{
		return MakeErrorResult(TEXT("Graph is no longer valid"));
	}

	// Get direction
	FString Direction;
	if (!Params->TryGetStringField(TEXT("direction"), Direction))
	{
		return MakeErrorResult(TEXT("Missing required field: direction"));
	}

	// Get current cursor node
	if (!Data->Cursor.FocusedNodeGuid.IsValid())
	{
		return MakeErrorResult(TEXT("Cursor is not focused on any node"));
	}

	UEdGraphNode* CurrentNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, Data->Cursor.FocusedNodeGuid);
	if (!CurrentNode)
	{
		return MakeErrorResult(TEXT("Current cursor node not found (may have been deleted)"));
	}

	UEdGraphNode* TargetNode = nullptr;
	FString MovementDescription;

	if (Direction == TEXT("right") || Direction == TEXT("exec_next"))
	{
		// Move to node connected via exec output pin
		TArray<UEdGraphPin*> ExecOutputs = ClaireonBlueprintHelpers::GetExecPins(CurrentNode, false, true);
		if (ExecOutputs.Num() > 0 && ExecOutputs[0]->LinkedTo.Num() > 0)
		{
			TargetNode = ExecOutputs[0]->LinkedTo[0]->GetOwningNode();
			MovementDescription = TEXT("exec flow");
		}
	}
	else if (Direction == TEXT("left") || Direction == TEXT("exec_prev"))
	{
		// Move to node connected via exec input pin
		TArray<UEdGraphPin*> ExecInputs = ClaireonBlueprintHelpers::GetExecPins(CurrentNode, true, false);
		if (ExecInputs.Num() > 0 && ExecInputs[0]->LinkedTo.Num() > 0)
		{
			TargetNode = ExecInputs[0]->LinkedTo[0]->GetOwningNode();
			MovementDescription = TEXT("reverse exec flow");
		}
	}
	else if (Direction == TEXT("up"))
	{
		// Find nearest node above
		float MinDistance = FLT_MAX;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node != CurrentNode && Node->NodePosY < CurrentNode->NodePosY)
			{
				float Distance = FMath::Abs(Node->NodePosX - CurrentNode->NodePosX) + (CurrentNode->NodePosY - Node->NodePosY);
				if (Distance < MinDistance)
				{
					MinDistance = Distance;
					TargetNode = Node;
				}
			}
		}
		MovementDescription = TEXT("spatial up");
	}
	else if (Direction == TEXT("down"))
	{
		// Find nearest node below
		float MinDistance = FLT_MAX;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Node && Node != CurrentNode && Node->NodePosY > CurrentNode->NodePosY)
			{
				float Distance = FMath::Abs(Node->NodePosX - CurrentNode->NodePosX) + (Node->NodePosY - CurrentNode->NodePosY);
				if (Distance < MinDistance)
				{
					MinDistance = Distance;
					TargetNode = Node;
				}
			}
		}
		MovementDescription = TEXT("spatial down");
	}
	else if (Direction == TEXT("next_pin"))
	{
		// Move to next pin on current node
		if (Data->Cursor.FocusedPinName != NAME_None)
		{
			bool FoundCurrent = false;
			for (UEdGraphPin* Pin : CurrentNode->Pins)
			{
				if (FoundCurrent && Pin->Direction == EGPD_Output)
				{
					Data->Cursor.FocusedPinName = Pin->PinName;
					Data->Cursor.FocusedPinDirection = Pin->Direction;
					Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Moved to next pin: %s"), *Pin->PinName.ToString());
					return BuildStateResponse(SessionId, Data);
				}
				if (Pin->PinName == Data->Cursor.FocusedPinName)
				{
					FoundCurrent = true;
				}
			}
		}
		return MakeErrorResult(TEXT("No next pin available"));
	}
	else if (Direction == TEXT("prev_pin"))
	{
		// Move to previous pin on current node
		if (Data->Cursor.FocusedPinName != NAME_None)
		{
			UEdGraphPin* PrevPin = nullptr;
			for (UEdGraphPin* Pin : CurrentNode->Pins)
			{
				if (Pin->PinName == Data->Cursor.FocusedPinName && PrevPin)
				{
					Data->Cursor.FocusedPinName = PrevPin->PinName;
					Data->Cursor.FocusedPinDirection = PrevPin->Direction;
					Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Moved to previous pin: %s"), *PrevPin->PinName.ToString());
					return BuildStateResponse(SessionId, Data);
				}
				if (Pin->Direction == EGPD_Output)
				{
					PrevPin = Pin;
				}
			}
		}
		return MakeErrorResult(TEXT("No previous pin available"));
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Unknown direction: %s (valid: right, left, up, down, exec_next, exec_prev, next_pin, prev_pin)"), *Direction));
	}

	if (!TargetNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("No node found in direction: %s"), *Direction));
	}

	// Move cursor to target node
	Data->Cursor.PushHistory();
	Data->Cursor.FocusedNodeGuid = TargetNode->NodeGuid;

	// Focus on first output pin
	UEdGraphPin* FirstOutputPin = ClaireonBlueprintHelpers::GetFirstOutputPin(TargetNode);
	if (FirstOutputPin)
	{
		Data->Cursor.FocusedPinName = FirstOutputPin->PinName;
		Data->Cursor.FocusedPinDirection = FirstOutputPin->Direction;
	}
	else
	{
		Data->Cursor.FocusedPinName = NAME_None;
	}

	FString TargetNodeTitle = TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Moved cursor %s to: %s"),
		*MovementDescription, *TargetNodeTitle);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_CursorBack(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UEdGraph* Graph = Data->Graph.Get();

	if (!Graph)
	{
		return MakeErrorResult(TEXT("Graph is no longer valid"));
	}

	// Pop from history
	FGuid PreviousNodeGuid;
	if (!Data->Cursor.PopHistory(PreviousNodeGuid))
	{
		return MakeErrorResult(TEXT("Cursor history is empty"));
	}

	// Find the previous node
	UEdGraphNode* PreviousNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, PreviousNodeGuid);
	if (!PreviousNode)
	{
		// Node was deleted, try to pop again
		if (!Data->Cursor.PopHistory(PreviousNodeGuid))
		{
			return MakeErrorResult(TEXT("No valid nodes in cursor history"));
		}

		PreviousNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, PreviousNodeGuid);
		if (!PreviousNode)
		{
			return MakeErrorResult(TEXT("All nodes in history have been deleted"));
		}
	}

	// Move cursor to previous node
	Data->Cursor.FocusedNodeGuid = PreviousNodeGuid;

	// Focus on first output pin
	UEdGraphPin* FirstOutputPin = ClaireonBlueprintHelpers::GetFirstOutputPin(PreviousNode);
	if (FirstOutputPin)
	{
		Data->Cursor.FocusedPinName = FirstOutputPin->PinName;
		Data->Cursor.FocusedPinDirection = FirstOutputPin->Direction;
	}
	else
	{
		Data->Cursor.FocusedPinName = NAME_None;
	}

	FString NodeTitle = PreviousNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Navigated back to: %s"), *NodeTitle);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_SelectNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UEdGraph* Graph = Data->Graph.Get();

	if (!Graph)
	{
		return MakeErrorResult(TEXT("Graph is no longer valid"));
	}

	// Get node_guid
	FString NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));
	}

	// Find the node
	UEdGraphNode* Node = FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	// Move cursor to this node
	Data->Cursor.PushHistory();
	Data->Cursor.FocusedNodeGuid = NodeGuid;

	// Focus on first output pin
	UEdGraphPin* FirstOutputPin = ClaireonBlueprintHelpers::GetFirstOutputPin(Node);
	if (FirstOutputPin)
	{
		Data->Cursor.FocusedPinName = FirstOutputPin->PinName;
		Data->Cursor.FocusedPinDirection = FirstOutputPin->Direction;
	}
	else
	{
		Data->Cursor.FocusedPinName = NAME_None;
	}

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Selected node: %s"), *NodeTitle);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_SelectPin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UEdGraph* Graph = Data->Graph.Get();

	if (!Graph)
	{
		return MakeErrorResult(TEXT("Graph is no longer valid"));
	}

	// Get node_guid and pin_name
	FString NodeGuidStr, PinName;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}
	if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
	{
		return MakeErrorResult(TEXT("Missing required field: pin_name"));
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));
	}

	// Find the node
	UEdGraphNode* Node = FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	// Resolve the pin using fuzzy matching
	TArray<FString> ResolutionWarnings;
	ClaireonNameResolver::FNameResolveResult SelectPinResult;
	UEdGraphPin* Pin = ClaireonNameResolver::ResolvePinName(Node, PinName, EGPD_MAX, SelectPinResult);
	if (!Pin)
	{
		return MakeErrorResult(SelectPinResult.Error);
	}
	if (!SelectPinResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(SelectPinResult.ResolutionNote);
	}

	// Move cursor to this pin
	Data->Cursor.PushHistory();
	Data->Cursor.FocusedNodeGuid = NodeGuid;
	Data->Cursor.FocusedPinName = Pin->PinName;
	Data->Cursor.FocusedPinDirection = Pin->Direction;

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	FString PinDir = (Pin->Direction == EGPD_Input) ? TEXT("input") : TEXT("output");
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Selected pin: [%s].%s (%s)"),
		*NodeTitle, *PinName, *PinDir);

	FToolResult SelectPinFinalResult = BuildStateResponse(SessionId, Data);
	SelectPinFinalResult.Warnings.Append(ResolutionWarnings);
	return SelectPinFinalResult;
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_SelectNearestNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UEdGraph* Graph = Data->Graph.Get();

	if (!Graph)
	{
		return MakeErrorResult(TEXT("Graph is no longer valid"));
	}

	// Get position
	const TSharedPtr<FJsonObject>* PositionObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("position"), PositionObj))
	{
		return MakeErrorResult(TEXT("Missing required field: position"));
	}

	double X = 0.0, Y = 0.0;
	if (!(*PositionObj)->TryGetNumberField(TEXT("x"), X) || !(*PositionObj)->TryGetNumberField(TEXT("y"), Y))
	{
		return MakeErrorResult(TEXT("Position must have 'x' and 'y' fields"));
	}

	FVector2D TargetPosition(X, Y);

	// Find nearest node
	UEdGraphNode* NearestNode = nullptr;
	float MinDistance = FLT_MAX;

	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (!Node)
		{
			continue;
		}

		FVector2D NodePosition(Node->NodePosX, Node->NodePosY);
		float Distance = FVector2D::Distance(TargetPosition, NodePosition);

		if (Distance < MinDistance)
		{
			MinDistance = Distance;
			NearestNode = Node;
		}
	}

	if (!NearestNode)
	{
		return MakeErrorResult(TEXT("No nodes found in graph"));
	}

	// Move cursor to nearest node
	Data->Cursor.PushHistory();
	Data->Cursor.FocusedNodeGuid = NearestNode->NodeGuid;

	// Focus on first output pin
	UEdGraphPin* FirstOutputPin = ClaireonBlueprintHelpers::GetFirstOutputPin(NearestNode);
	if (FirstOutputPin)
	{
		Data->Cursor.FocusedPinName = FirstOutputPin->PinName;
		Data->Cursor.FocusedPinDirection = FirstOutputPin->Direction;
	}
	else
	{
		Data->Cursor.FocusedPinName = NAME_None;
	}

	FString NodeTitle = NearestNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Selected nearest node: %s (distance: %.1f)"),
		*NodeTitle, MinDistance);

	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_GetState(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	Data->Cursor.LastOperationStatus = TEXT("Retrieved current state (no modifications)");
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_ImportNodes(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
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
			Data->Cursor.PushHistory();
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

FToolResult ClaireonTool_EditBlueprintGraph::Operation_Compile(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	// Compile the Blueprint
	FKismetEditorUtilities::CompileBlueprint(Blueprint);

	// Check compilation status
	EBlueprintStatus Status = Blueprint->Status;
	FString StatusText;

	switch (Status)
	{
		case BS_UpToDate:
		case BS_UpToDateWithWarnings:
			StatusText = TEXT("Compilation successful");
			if (Status == BS_UpToDateWithWarnings)
			{
				StatusText += TEXT(" (with warnings)");
			}
			break;

		case BS_Error:
			StatusText = TEXT("Compilation failed with errors");
			break;

		case BS_Unknown:
		case BS_Dirty:
			StatusText = TEXT("Compilation status unknown");
			break;

		default:
			StatusText = TEXT("Compilation completed");
			break;
	}

	// TODO: Extract compiler messages from Blueprint compiler log
	// UE5 doesn't expose CompilerResults directly on UBlueprint

	Data->Cursor.LastOperationStatus = StatusText;
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_Save(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	if (!Blueprint)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[EditBlueprintGraph] Save: Blueprint is no longer valid"));
		return MakeErrorResult(TEXT("Blueprint is no longer valid"));
	}

	UPackage* Package = Blueprint->GetOutermost();
	if (!Package)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[EditBlueprintGraph] Save: Failed to get package for Blueprint"));
		return MakeErrorResult(TEXT("Failed to get package for Blueprint"));
	}

	// Compile the Blueprint to ensure it's in a valid state before saving
	// This initializes the generated class and ensures the Blueprint is complete
	UE_LOG(LogClaireon, Log, TEXT("[EditBlueprintGraph] Save: Compiling Blueprint before save"));
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

	// Ensure package is properly configured for saving
	Package->SetIsExternallyReferenceable(true);
	Package->MarkPackageDirty();

	// Save package
	FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

	UE_LOG(LogClaireon, Log, TEXT("[EditBlueprintGraph] Save: Attempting to save to %s"), *PackageFileName);

	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	}
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_None; // Report errors - we expect save to succeed now

	if (UPackage::SavePackage(Package, Blueprint, *PackageFileName, SaveArgs))
	{
		UE_LOG(LogClaireon, Log, TEXT("[EditBlueprintGraph] Save: Successfully saved Blueprint to %s"), *PackageFileName);
		Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Saved Blueprint to %s"), *PackageFileName);
		return BuildStateResponse(SessionId, Data);
	}
	else
	{
		UE_LOG(LogClaireon, Error, TEXT("[EditBlueprintGraph] Save: Failed to save Blueprint to %s"), *PackageFileName);
		return MakeErrorResult(FString::Printf(TEXT("Failed to save Blueprint to %s"), *PackageFileName));
	}
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_Format(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
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

FToolResult ClaireonTool_EditBlueprintGraph::Operation_Close(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FClaireonSessionManager::Get().CloseSession(SessionId);
	// ToolData cleanup happens via HandleSessionClosed delegate
	return MakeSuccessResult(nullptr, TEXT("Session closed successfully"));
}

// ============================================================================
// Helpers
// ============================================================================

FToolResult ClaireonTool_EditBlueprintGraph::BuildStateResponse(const FString& SessionId, FBlueprintEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Invalid session"));
	}

	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	// Validate cursor
	ValidateCursor(Data);

	// =========================================================================
	// Determine effective response mode
	// =========================================================================
	// Legacy: bSuppressOutput maps to "status"
	FString EffectiveMode = Data->ResponseMode;
	if (Data->bSuppressOutput && EffectiveMode == TEXT("changed"))
	{
		EffectiveMode = TEXT("status");
	}

	// "changed" falls back to "status" when no affected nodes were recorded
	// (e.g. non-connectivity ops like save/format, or open which bypasses this path)
	if (EffectiveMode == TEXT("changed") && Data->LastOperationAffectedNodes.IsEmpty())
	{
		EffectiveMode = TEXT("status");
	}

	// =========================================================================
	// Surface GUID corrections so the MCP client can update stale references
	// =========================================================================
	FString GuidCorrectionNote;
	if (Data->GuidCorrections.Num() > 0)
	{
		GuidCorrectionNote = TEXT("\n\n## GUID Corrections (blueprint was recompiled — update your references)\n");
		for (const auto& Pair : Data->GuidCorrections)
		{
			GuidCorrectionNote += FString::Printf(TEXT("  %s → %s\n"),
				*Pair.Key.ToString(), *Pair.Value.ToString());
		}
		Data->GuidCorrections.Empty();
	}

	// =========================================================================
	// "status" mode — brief status line only
	// =========================================================================
	if (EffectiveMode == TEXT("status"))
	{
		FString StatusMsg = Data->Cursor.LastOperationStatus.IsEmpty() ? TEXT("ok") : FString::Printf(TEXT("ok: %s"), *Data->Cursor.LastOperationStatus);
		return MakeSuccessResult(nullptr, StatusMsg + GuidCorrectionNote);
	}

	// =========================================================================
	// "changed" mode — pin-level diff of affected nodes
	// =========================================================================
	if (EffectiveMode == TEXT("changed"))
	{
		int32 TotalNodes = Graph->Nodes.Num();
		int32 AffectedCount = Data->LastOperationAffectedNodes.Num();

		FString DiffText;

		// Status header
		if (!Data->Cursor.LastOperationStatus.IsEmpty())
		{
			DiffText += FString::Printf(TEXT("## Status\n%s\n\n"), *Data->Cursor.LastOperationStatus);
		}

		DiffText += FString::Printf(TEXT("## Changed nodes (%d of %d):\n\n"), AffectedCount, TotalNodes);

		for (const FGuid& AffGuid : Data->LastOperationAffectedNodes)
		{
			// Find the node in the current graph
			UEdGraphNode* AffNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, AffGuid);
			if (!AffNode)
			{
				// Node was removed — we can't show its current state; skip
				continue;
			}

			FString NodeTitle = AffNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
			FString NodeClass = AffNode->GetClass()->GetName();

			DiffText += FString::Printf(TEXT("[%s] (%s) [GUID: %s]\n"),
				*NodeTitle, *NodeClass, *AffGuid.ToString());

			// Per-pin diff
			bool bAnyPinDiff = false;
			const TMap<FName, TArray<FString>>* PrePinMap = Data->PreOpPinConnections.Find(AffGuid);

			for (UEdGraphPin* DiffPin : AffNode->Pins)
			{
				if (!DiffPin)
				{
					continue;
				}

				// Current connections for this pin
				TArray<FString> CurrentConnected;
				for (UEdGraphPin* LinkedDiff : DiffPin->LinkedTo)
				{
					if (LinkedDiff && LinkedDiff->GetOwningNode())
					{
						CurrentConnected.Add(LinkedDiff->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
					}
				}

				// Pre-op connections for this pin
				TArray<FString> PreviousConnected;
				if (PrePinMap)
				{
					const TArray<FString>* PreConns = PrePinMap->Find(DiffPin->PinName);
					if (PreConns)
					{
						PreviousConnected = *PreConns;
					}
				}

				// Find added connections (in current but not in previous)
				for (const FString& CurConn : CurrentConnected)
				{
					if (!PreviousConnected.Contains(CurConn))
					{
						FString DirArrow = (DiffPin->Direction == EGPD_Output) ? TEXT("->") : TEXT("<-");
						DiffText += FString::Printf(TEXT("  ADDED:   %s(%s) %s [%s]\n"),
							*DiffPin->PinName.ToString(),
							*DiffPin->PinType.PinCategory.ToString(),
							*DirArrow,
							*CurConn);
						bAnyPinDiff = true;
					}
				}

				// Find removed connections (in previous but not in current)
				for (const FString& PrevConn : PreviousConnected)
				{
					if (!CurrentConnected.Contains(PrevConn))
					{
						FString DirArrow = (DiffPin->Direction == EGPD_Output) ? TEXT("->") : TEXT("<-");
						if (CurrentConnected.Num() == 0)
						{
							DiffText += FString::Printf(TEXT("  REMOVED: %s(%s) %s [%s] (now unconnected)\n"),
								*DiffPin->PinName.ToString(),
								*DiffPin->PinType.PinCategory.ToString(),
								*DirArrow,
								*PrevConn);
						}
						else
						{
							DiffText += FString::Printf(TEXT("  REMOVED: %s(%s) %s [%s]\n"),
								*DiffPin->PinName.ToString(),
								*DiffPin->PinType.PinCategory.ToString(),
								*DirArrow,
								*PrevConn);
						}
						bAnyPinDiff = true;
					}
				}
			}

			if (!bAnyPinDiff)
			{
				DiffText += TEXT("  (exec connections unchanged)\n");
			}

			DiffText += TEXT("\n");
		}

		DiffText += FString::Printf(
			TEXT("(Full graph: %d nodes. Use editor.blueprint.getGraph to see all.)"),
			TotalNodes);

		return MakeSuccessResult(nullptr, DiffText + GuidCorrectionNote);
	}

	// =========================================================================
	// "full" mode — full graph state (JSON + T3D); also the fallback
	// =========================================================================
	{
		// Part 1: Operation status + Cursor info + Graph state summary
		FString StatusText;

		// Operation status
		if (!Data->Cursor.LastOperationStatus.IsEmpty())
		{
			StatusText += FString::Printf(TEXT("## Status\n%s\n\n"), *Data->Cursor.LastOperationStatus);
		}

		// Session info
		StatusText += FString::Printf(TEXT("## Session\nSession ID: %s\nBlueprint: %s\nGraph: %s\n\n"),
			*SessionId,
			*Blueprint->GetPathName(),
			*Graph->GetName());

		// Cursor info
		StatusText += TEXT("## Cursor\n");
		if (Data->Cursor.FocusedNodeGuid.IsValid())
		{
			UEdGraphNode* FocusedNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, Data->Cursor.FocusedNodeGuid);
			if (FocusedNode)
			{
				StatusText += FString::Printf(TEXT("Focused Node: %s [GUID: %s]\n"),
					*FocusedNode->GetNodeTitle(ENodeTitleType::ListView).ToString(),
					*Data->Cursor.FocusedNodeGuid.ToString());

				if (Data->Cursor.FocusedPinName != NAME_None)
				{
					UEdGraphPin* FocusedPin = FocusedNode->FindPin(Data->Cursor.FocusedPinName, Data->Cursor.FocusedPinDirection);
					if (FocusedPin)
					{
						FString PinDir = (FocusedPin->Direction == EGPD_Input) ? TEXT("input") : TEXT("output");
						StatusText += FString::Printf(TEXT("Focused Pin: %s (%s, %s)\n"),
							*FocusedPin->PinName.ToString(),
							*PinDir,
							*FocusedPin->PinType.PinCategory.ToString());
					}
				}

				StatusText += FString::Printf(TEXT("Position: (%.0f, %.0f)\n"), CLAIREON_GET_NODE_POS_X(FocusedNode), CLAIREON_GET_NODE_POS_Y(FocusedNode));
			}
			else
			{
				StatusText += TEXT("Focused Node: (invalid)\n");
			}
		}
		else
		{
			StatusText += TEXT("Focused Node: (none)\n");
		}
		StatusText += TEXT("\n");

		// Graph state summary
		StatusText += FString::Printf(TEXT("## Graph State: %s (%d nodes)\n\n"), *Graph->GetName(), Graph->Nodes.Num());

		// List nodes with simple format
		int32 NodeIndex = 1;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!Node)
			{
				continue;
			}

			FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			bool bIsCursor = (Node->NodeGuid == Data->Cursor.FocusedNodeGuid);

			StatusText += FString::Printf(TEXT("%d. [%s] @ (%.0f, %.0f)%s\n"),
				NodeIndex++,
				*NodeTitle,
				Node->GetNodePosX(),
				Node->GetNodePosY(),
				bIsCursor ? TEXT(" <<<CURSOR>>>") : TEXT(""));

			// Show execution connections (simplified)
			TArray<UEdGraphPin*> ExecOutputs = ClaireonBlueprintHelpers::GetExecPins(Node, false, true);
			for (UEdGraphPin* ExecPin : ExecOutputs)
			{
				if (ExecPin->LinkedTo.Num() > 0)
				{
					for (UEdGraphPin* LinkedPin : ExecPin->LinkedTo)
					{
						if (LinkedPin && LinkedPin->GetOwningNode())
						{
							FString LinkedTitle = LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::ListView).ToString();
							StatusText += FString::Printf(TEXT("   -> exec -> [%s]\n"), *LinkedTitle);
						}
					}
				}
			}
		}

		// Part 2: T3D export (if nodes exist)
		if (Graph->Nodes.Num() > 0)
		{
			// Convert TArray to TSet for FEdGraphUtilities::ExportNodesToText
			TSet<UObject*> NodeSet;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (Node)
				{
					NodeSet.Add(Node);
				}
			}

			FString T3DText;
			FEdGraphUtilities::ExportNodesToText(NodeSet, T3DText);

			if (!T3DText.IsEmpty())
			{
				StatusText += FString::Printf(TEXT("\n## T3D Export\n\n```\n%s\n```"), *T3DText);
			}
		}

		return MakeSuccessResult(nullptr, StatusText + GuidCorrectionNote);
	}
}

void ClaireonTool_EditBlueprintGraph::ValidateCursor(FBlueprintEditToolData* Data)
{
	if (!Data || !Data->Graph.IsValid())
	{
		return;
	}

	// Check if focused node still exists
	if (Data->Cursor.FocusedNodeGuid.IsValid())
	{
		UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Data->Graph.Get(), Data->Cursor.FocusedNodeGuid);
		if (!Node)
		{
			// Node was deleted, reset cursor to first root node
			TArray<UEdGraphNode*> RootNodes = ClaireonBlueprintHelpers::FindRootNodes(Data->Graph.Get());
			if (RootNodes.Num() > 0)
			{
				Data->Cursor.FocusedNodeGuid = RootNodes[0]->NodeGuid;
				UEdGraphPin* FirstOutput = ClaireonBlueprintHelpers::GetFirstOutputPin(RootNodes[0]);
				if (FirstOutput)
				{
					Data->Cursor.FocusedPinName = FirstOutput->PinName;
					Data->Cursor.FocusedPinDirection = FirstOutput->Direction;
				}
			}
			else
			{
				// No nodes left, reset cursor
				Data->Cursor.FocusedNodeGuid = FGuid();
				Data->Cursor.FocusedPinName = NAME_None;
			}
		}
	}
}

// ============================================================================
// Stage 001: list_graphs (stateless)
// ============================================================================

FToolResult ClaireonTool_EditBlueprintGraph::Operation_ListGraphs(const TSharedPtr<FJsonObject>& Params)
{
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
	if (!Blueprint)
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
		if (Graph)
			Graphs.Add({ Graph->GetName(), TEXT("Ubergraph"), Graph->Nodes.Num() });
	}
	for (UEdGraph* Graph : Blueprint->FunctionGraphs)
	{
		if (!Graph)
			continue;
		// UAnimationGraph instances in FunctionGraphs are anim graphs, not regular functions
		const FString GraphType = Cast<UAnimationGraph>(Graph) ? TEXT("AnimGraph") : TEXT("Function");
		Graphs.Add({ Graph->GetName(), GraphType, Graph->Nodes.Num() });
	}
	for (UEdGraph* Graph : Blueprint->MacroGraphs)
	{
		if (Graph)
			Graphs.Add({ Graph->GetName(), TEXT("Macro"), Graph->Nodes.Num() });
	}
	for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
	{
		if (Graph)
			Graphs.Add({ Graph->GetName(), TEXT("DelegateSignature"), Graph->Nodes.Num() });
	}

	FString Output = FString::Printf(TEXT("Graphs in %s (%d total):\n"), *AssetPath, Graphs.Num());
	for (const FGraphEntry& Entry : Graphs)
	{
		Output += FString::Printf(TEXT("  %s  [%s, %d nodes]\n"),
			*Entry.Name, *Entry.Type, Entry.NodeCount);
	}

	return MakeSuccessResult(nullptr, Output);
}

// ============================================================================
// Stage 002: Stateless remove_node + reconstruct_node (session and stateless)
// ============================================================================

FToolResult ClaireonTool_EditBlueprintGraph::Operation_RemoveNodeStateless(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, GraphName, NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
		return MakeErrorResult(TEXT("Missing required field: asset_path. Stateless remove_node requires: asset_path, graph_name, node_guid"));
	if (!Params->TryGetStringField(TEXT("graph_name"), GraphName))
		return MakeErrorResult(TEXT("Missing required field: graph_name. Stateless remove_node requires: asset_path, graph_name, node_guid"));
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid. Stateless remove_node requires: asset_path, graph_name, node_guid"));

	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
		return MakeErrorResult(ValidationError);

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));

	UEdGraph* Graph = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, GraphName);
	if (!Graph)
		return MakeErrorResult(FString::Printf(TEXT("Graph '%s' not found"), *GraphName));

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));

	UEdGraphNode* Node = FindNodeForOperation(Graph, NodeGuid, nullptr);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Blueprint Node")));
	Blueprint->Modify();
	Graph->Modify();

	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
			Pin->BreakAllPinLinks();
	}
	Graph->RemoveNode(Node);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	return MakeSuccessResult(nullptr, FString::Printf(TEXT("Removed node '%s' (GUID: %s) from %s/%s"), *NodeTitle, *NodeGuidStr, *AssetPath, *GraphName));
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_ReconstructNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();
	if (!Blueprint || !Graph)
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));

	FString NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));

	UEdGraphNode* Node = FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Reconstruct Blueprint Node")));
	Blueprint->Modify();
	Node->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Reconstructed node: %s"), *NodeTitle);
	return BuildStateResponse(SessionId, Data);
}

FToolResult ClaireonTool_EditBlueprintGraph::Operation_ReconstructNodeStateless(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, GraphName, NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
		return MakeErrorResult(TEXT("Missing required field: asset_path. Stateless reconstruct_node requires: asset_path, graph_name, node_guid"));
	if (!Params->TryGetStringField(TEXT("graph_name"), GraphName))
		return MakeErrorResult(TEXT("Missing required field: graph_name. Stateless reconstruct_node requires: asset_path, graph_name, node_guid"));
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid. Stateless reconstruct_node requires: asset_path, graph_name, node_guid"));

	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
		return MakeErrorResult(ValidationError);

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));

	UEdGraph* Graph = ClaireonBlueprintHelpers::FindGraphByName(Blueprint, GraphName);
	if (!Graph)
		return MakeErrorResult(FString::Printf(TEXT("Graph '%s' not found"), *GraphName));

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));

	UEdGraphNode* Node = FindNodeForOperation(Graph, NodeGuid, nullptr);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Reconstruct Blueprint Node")));
	Blueprint->Modify();
	Node->ReconstructNode();
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	return MakeSuccessResult(nullptr, FString::Printf(TEXT("Reconstructed node '%s' (GUID: %s) in %s/%s. Compile to apply."), *NodeTitle, *NodeGuidStr, *AssetPath, *GraphName));
}

// ============================================================================
// add_pin — Add dynamic pins to nodes with IK2Node_AddPinInterface or Switch
// ============================================================================

FToolResult ClaireonTool_EditBlueprintGraph::Operation_AddPin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();
	if (!Blueprint || !Graph)
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));

	FString NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));

	UEdGraphNode* Node = FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s.\n%s"), *NodeGuidStr, *AvailableNodes));
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
	else if (UK2Node_Switch* SwitchNode = Cast<UK2Node_Switch>(Node))
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
				if (UK2Node_SwitchString* StringSwitch = Cast<UK2Node_SwitchString>(SwitchNode))
				{
					StringSwitch->PinNames.Add(FName(*PinValue));
					// Append index suffix for subsequent pins in batch
					if (i > 0)
					{
						StringSwitch->PinNames.Last() = FName(*FString::Printf(TEXT("%s_%d"), *PinValue, i));
					}
				}
				else if (UK2Node_SwitchName* NameSwitch = Cast<UK2Node_SwitchName>(SwitchNode))
				{
					NameSwitch->PinNames.Add(FName(*PinValue));
					if (i > 0)
					{
						NameSwitch->PinNames.Last() = FName(*FString::Printf(TEXT("%s_%d"), *PinValue, i));
					}
				}
				else
				{
					// Integer switch — ignore pin_value, use auto-numbering
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

// ============================================================================
// remove_pin — Remove dynamic pins from nodes
// ============================================================================

FToolResult ClaireonTool_EditBlueprintGraph::Operation_RemovePin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();
	if (!Blueprint || !Graph)
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));

	FString NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));

	UEdGraphNode* Node = FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s.\n%s"), *NodeGuidStr, *AvailableNodes));
	}

	// Find the pin to remove — by name or by index
	FString PinName;
	UEdGraphPin* TargetPin = nullptr;

	TArray<FString> ResolutionWarnings;
	if (Params->TryGetStringField(TEXT("pin_name"), PinName))
	{
		ClaireonNameResolver::FNameResolveResult RemovePinResult;
		TargetPin = ClaireonNameResolver::ResolvePinName(Node, PinName, EGPD_MAX, RemovePinResult);
		if (!TargetPin)
		{
			return MakeErrorResult(RemovePinResult.Error);
		}
		if (!RemovePinResult.ResolutionNote.IsEmpty())
		{
			ResolutionWarnings.Add(RemovePinResult.ResolutionNote);
		}
	}
	else
	{
		int32 PinIndex = -1;
		if (!Params->TryGetNumberField(TEXT("pin_index"), PinIndex))
		{
			return MakeErrorResult(TEXT("Missing required field: pin_name or pin_index"));
		}

		// Collect dynamic/user-added pins (output exec pins for Sequence/Switch, input data pins for containers)
		TArray<UEdGraphPin*> DynamicPins;
		IK2Node_AddPinInterface* AddPinIface = Cast<IK2Node_AddPinInterface>(Node);
		UK2Node_Switch* SwitchNode = Cast<UK2Node_Switch>(Node);

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->ParentPin != nullptr)
				continue;

			if (AddPinIface && AddPinIface->CanRemovePin(Pin))
			{
				DynamicPins.Add(Pin);
			}
			else if (SwitchNode)
			{
				// For switch nodes, dynamic pins are output exec pins that aren't the default pin
				UEdGraphPin* DefaultPin = SwitchNode->GetDefaultPin();
				if (Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin != DefaultPin)
				{
					DynamicPins.Add(Pin);
				}
			}
		}

		if (PinIndex < 0 || PinIndex >= DynamicPins.Num())
		{
			return MakeErrorResult(FString::Printf(TEXT("pin_index %d out of range. Node has %d removable pins."), PinIndex, DynamicPins.Num()));
		}
		TargetPin = DynamicPins[PinIndex];
	}

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	FString RemovedPinName = TargetPin->PinName.ToString();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Blueprint Pin")));
	Blueprint->Modify();

	// Dispatch removal
	IK2Node_AddPinInterface* AddPinIface = Cast<IK2Node_AddPinInterface>(Node);
	if (AddPinIface)
	{
		if (!AddPinIface->CanRemovePin(TargetPin))
		{
			return MakeErrorResult(FString::Printf(TEXT("Cannot remove pin '%s' from node '%s'"), *RemovedPinName, *NodeTitle));
		}
		AddPinIface->RemoveInputPin(TargetPin);
	}
	else if (UK2Node_Switch* SwitchNode = Cast<UK2Node_Switch>(Node))
	{
		if (SwitchNode->IsA<UK2Node_SwitchEnum>())
		{
			return MakeErrorResult(TEXT("SwitchEnum pins cannot be removed (fixed to enum entries)"));
		}
		SwitchNode->RemovePinFromSwitchNode(TargetPin);
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Node '%s' does not support removing pins"), *NodeTitle));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Removed pin '%s' from node: %s"), *RemovedPinName, *NodeTitle);
	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
	FToolResult RemovePinFinalResult = BuildStateResponse(SessionId, Data);
	RemovePinFinalResult.Warnings.Append(ResolutionWarnings);
	return RemovePinFinalResult;
}

// ============================================================================
// split_pin — Split a struct pin into its component members
// ============================================================================

FToolResult ClaireonTool_EditBlueprintGraph::Operation_SplitPin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();
	if (!Blueprint || !Graph)
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));

	FString NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));

	FString PinName;
	if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
		return MakeErrorResult(TEXT("Missing required field: pin_name"));

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));

	UEdGraphNode* Node = FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s.\n%s"), *NodeGuidStr, *AvailableNodes));
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

	if (!Node->CanSplitPin(Pin))
	{
		return MakeErrorResult(FString::Printf(TEXT("Pin '%s' cannot be split (not a struct pin or already split)"), *PinName));
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

// ============================================================================
// recombine_pin — Recombine a previously split struct pin
// ============================================================================

FToolResult ClaireonTool_EditBlueprintGraph::Operation_RecombinePin(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();
	if (!Blueprint || !Graph)
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));

	FString NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));

	FString PinName;
	if (!Params->TryGetStringField(TEXT("pin_name"), PinName))
		return MakeErrorResult(TEXT("Missing required field: pin_name"));

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));

	UEdGraphNode* Node = FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s.\n%s"), *NodeGuidStr, *AvailableNodes));
	}

	TArray<FString> ResolutionWarnings;
	ClaireonNameResolver::FNameResolveResult RecombinePinResult;
	UEdGraphPin* Pin = ClaireonNameResolver::ResolvePinName(Node, PinName, EGPD_MAX, RecombinePinResult);
	if (!Pin)
	{
		return MakeErrorResult(RecombinePinResult.Error);
	}
	if (!RecombinePinResult.ResolutionNote.IsEmpty())
	{
		ResolutionWarnings.Add(RecombinePinResult.ResolutionNote);
	}

	if (Pin->SubPins.Num() == 0)
	{
		return MakeErrorResult(FString::Printf(TEXT("Pin '%s' is not currently split"), *PinName));
	}

	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Recombine Blueprint Pin")));
	Blueprint->Modify();

	K2Schema->RecombinePin(Pin);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Recombined pin '%s' on node '%s'"), *PinName, *NodeTitle);
	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
	FToolResult RecombineFinalResult = BuildStateResponse(SessionId, Data);
	RecombineFinalResult.Warnings.Append(ResolutionWarnings);
	return RecombineFinalResult;
}

// ============================================================================
// set_gameplay_tags (stateless)
// ============================================================================

FToolResult ClaireonTool_EditBlueprintGraph::Operation_SetGameplayTags(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath, PropertyPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath))
		return MakeErrorResult(TEXT("Missing required field: asset_path. set_gameplay_tags requires: asset_path, property_path, tags_to_add (array), tags_to_remove (array)"));
	if (!Params->TryGetStringField(TEXT("property_path"), PropertyPath))
		return MakeErrorResult(TEXT("Missing required field: property_path. set_gameplay_tags requires: asset_path, property_path, tags_to_add (array), tags_to_remove (array)"));

	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
		return MakeErrorResult(ValidationError);

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *AssetPath);
	if (!Blueprint)
		return MakeErrorResult(FString::Printf(TEXT("Failed to load Blueprint: %s"), *AssetPath));

	if (!Blueprint->GeneratedClass)
		return MakeErrorResult(TEXT("Blueprint has no GeneratedClass (compile it first)"));

	UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
	if (!CDO)
		return MakeErrorResult(TEXT("Failed to get Blueprint CDO"));

	// Parse tags_to_add and tags_to_remove arrays
	TArray<FString> TagsToAdd, TagsToRemove;
	{
		const TArray<TSharedPtr<FJsonValue>>* AddArray = nullptr;
		if (Params->TryGetArrayField(TEXT("tags_to_add"), AddArray))
		{
			for (const TSharedPtr<FJsonValue>& Val : *AddArray)
			{
				FString TagStr;
				if (Val->TryGetString(TagStr) && !TagStr.IsEmpty())
					TagsToAdd.Add(TagStr);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* RemoveArray = nullptr;
		if (Params->TryGetArrayField(TEXT("tags_to_remove"), RemoveArray))
		{
			for (const TSharedPtr<FJsonValue>& Val : *RemoveArray)
			{
				FString TagStr;
				if (Val->TryGetString(TagStr) && !TagStr.IsEmpty())
					TagsToRemove.Add(TagStr);
			}
		}
	}

	if (TagsToAdd.IsEmpty() && TagsToRemove.IsEmpty())
		return MakeErrorResult(TEXT("At least one of tags_to_add or tags_to_remove must be non-empty"));

	// Walk the dot-separated property path to reach the FGameplayTagContainer
	// e.g. "RemovalTagRequirements.require_tags" -> walk two levels of FStructProperty
	TArray<FString> PathParts;
	PropertyPath.ParseIntoArray(PathParts, TEXT("."));

	void* CurrentContainer = CDO;
	UStruct* CurrentStruct = CDO->GetClass();
	FProperty* FinalProperty = nullptr;
	void* FinalContainer = nullptr;

	for (int32 i = 0; i < PathParts.Num(); i++)
	{
		FProperty* Prop = CurrentStruct->FindPropertyByName(FName(*PathParts[i]));
		if (!Prop)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Property '%s' not found on '%s'"),
				*PathParts[i], *CurrentStruct->GetName()));
		}

		if (i == PathParts.Num() - 1)
		{
			FinalProperty = Prop;
			FinalContainer = CurrentContainer;
		}
		else
		{
			// Must be a struct property to continue walking
			FStructProperty* StructProp = CastField<FStructProperty>(Prop);
			if (!StructProp)
			{
				return MakeErrorResult(FString::Printf(
					TEXT("Property '%s' is not a struct — cannot walk further"), *PathParts[i]));
			}
			CurrentContainer = StructProp->ContainerPtrToValuePtr<void>(CurrentContainer);
			CurrentStruct = StructProp->Struct;
		}
	}

	// Verify final property is a FGameplayTagContainer
	FStructProperty* TagContainerProp = CastField<FStructProperty>(FinalProperty);
	if (!TagContainerProp || TagContainerProp->Struct != TBaseStructure<FGameplayTagContainer>::Get())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Property '%s' is not a FGameplayTagContainer"), *PathParts.Last()));
	}

	FGameplayTagContainer* Container =
		TagContainerProp->ContainerPtrToValuePtr<FGameplayTagContainer>(FinalContainer);

	// Check for deprecated property — surface as warning, not error
	FString WarningText;
	if (FinalProperty->HasAnyPropertyFlags(CPF_Deprecated))
	{
		FString DeprecationMsg = FinalProperty->GetMetaData(TEXT("DeprecationMessage"));
		WarningText = FString::Printf(
			TEXT("[DEPRECATED] Property '%s' is deprecated. %s\n"),
			*FinalProperty->GetName(),
			DeprecationMsg.IsEmpty() ? TEXT("No replacement hint available.") : *DeprecationMsg);
	}

	// Apply changes in a transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Blueprint Gameplay Tags")));
	Blueprint->Modify();
	CDO->Modify();

	UGameplayTagsManager& TagsManager = UGameplayTagsManager::Get();

	for (const FString& TagName : TagsToRemove)
	{
		FGameplayTag Tag = TagsManager.RequestGameplayTag(FName(*TagName), /*bErrorIfNotFound=*/false);
		if (Tag.IsValid())
		{
			Container->RemoveTag(Tag);
		}
		else
		{
			WarningText += FString::Printf(TEXT("[WARN] Tag '%s' not registered in project — skipped removal\n"), *TagName);
		}
	}

	for (const FString& TagName : TagsToAdd)
	{
		FGameplayTag Tag = TagsManager.RequestGameplayTag(FName(*TagName), /*bErrorIfNotFound=*/false);
		if (Tag.IsValid())
		{
			Container->AddTag(Tag);
		}
		else
		{
			WarningText += FString::Printf(TEXT("[WARN] Tag '%s' not registered in project — skipped add\n"), *TagName);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Report resulting container contents
	TArray<FString> ResultTags;
	for (const FGameplayTag& Tag : Container->GetGameplayTagArray())
	{
		ResultTags.Add(Tag.ToString());
	}

	FString Output = WarningText;
	Output += FString::Printf(TEXT("Updated %s.%s\n"), *AssetPath, *PropertyPath);
	Output += FString::Printf(TEXT("Added: %s\nRemoved: %s\nResulting tags: [%s]"),
		*FString::Join(TagsToAdd, TEXT(", ")),
		*FString::Join(TagsToRemove, TEXT(", ")),
		*FString::Join(ResultTags, TEXT(", ")));

	return MakeSuccessResult(nullptr, Output);
}

// ============================================================================
// Operation: move_node
// ============================================================================

FToolResult ClaireonTool_EditBlueprintGraph::Operation_MoveNode(const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();

	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph is no longer valid"));
	}

	// Get node_guid
	FString NodeGuidStr;
	if (!Params->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}

	FGuid NodeGuid;
	if (!FGuid::Parse(NodeGuidStr, NodeGuid))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid node_guid format: %s"), *NodeGuidStr));
	}

	// Get position
	const TSharedPtr<FJsonObject>* PositionObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("position"), PositionObj))
	{
		return MakeErrorResult(TEXT("Missing required field: position (object with x, y)"));
	}

	double X = 0.0, Y = 0.0;
	(*PositionObj)->TryGetNumberField(TEXT("x"), X);
	(*PositionObj)->TryGetNumberField(TEXT("y"), Y);

	// Find the node
	UEdGraphNode* Node = FindNodeForOperation(Graph, NodeGuid, Data);
	if (!Node)
	{
		FString AvailableNodes = ClaireonBlueprintHelpers::FormatAvailableNodes(Graph);
		return MakeErrorResult(FString::Printf(TEXT("Node not found with GUID: %s in graph '%s'.\n%s"),
			*NodeGuidStr, *Graph->GetName(), *AvailableNodes));
	}

	// Move the node
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Move Blueprint Node")));
	Node->Modify();
	Node->NodePosX = X;
	Node->NodePosY = Y;
	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// Operation: add_function_override
// ============================================================================

FToolResult ClaireonTool_EditBlueprintGraph::Operation_AddFunctionOverride(
	const FString& SessionId, FBlueprintEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
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

		// Switch session to the new function graph
		Data->Graph = NewGraph;
		Data->Cursor.GraphName = NewGraph->GetName();
		Data->Cursor.PushHistory();
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
		Data->Cursor.PushHistory();
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

// ============================================================================
// Operation: apply_spec
// ============================================================================

FToolResult ClaireonTool_EditBlueprintGraph::Operation_ApplySpec(const TSharedPtr<FJsonObject>& Params)
{
	// Extract asset_path -- required
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("apply_spec requires 'asset_path' parameter"));
	}

	// Extract spec -- required JSON object
	const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
	{
		return MakeErrorResult(TEXT("apply_spec requires 'spec' parameter (JSON object)"));
	}

	// Optional: reuse an existing session
	FString SessionId;
	Params->TryGetStringField(TEXT("session_id"), SessionId);

	FClaireonSpecApplicator_Blueprint Applicator;
	return Applicator.ApplySpec(*SpecPtr, AssetPath, SessionId);
}

#undef LOCTEXT_NAMESPACE
