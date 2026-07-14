// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimGraphTools_Node.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Tools/ClaireonAnimGraphEditBase.h"
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonNameResolver.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLog.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_Base.h"
#include "AnimationGraph.h"
#include "K2Node.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "ScopedTransaction.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

#define LOCTEXT_NAMESPACE "ClaireonAnimGraphTools_Node"

namespace ClaireonAnimGraphTools_NodeInternal
{

// ============================================================================
// Helper: Find node by GUID or title
// ============================================================================

UEdGraphNode* FindNodeByGuidOrTitle(UEdGraph* Graph, const FString& Identifier, FString& OutError)
{
	// Try GUID first
	FGuid ParsedGuid;
	if (FGuid::Parse(Identifier, ParsedGuid) && ParsedGuid.IsValid())
	{
		UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, ParsedGuid);
		if (Node) return Node;
	}

	// Fall back to title match
	TArray<UEdGraphNode*> Matches = ClaireonBlueprintHelpers::FindNodesByTitle(Graph, Identifier);
	if (Matches.Num() == 1)
	{
		return Matches[0];
	}
	if (Matches.Num() > 1)
	{
		OutError = FString::Printf(TEXT("Multiple nodes match title '%s' — use GUID instead"), *Identifier);
		return nullptr;
	}

	OutError = FString::Printf(TEXT("Node '%s' not found in graph '%s'"), *Identifier, *Graph->GetName());
	return nullptr;
}

// ============================================================================
// Helper: Find pin by name on a node
// ============================================================================

UEdGraphPin* FindPinByName(UEdGraphNode* Node, const FString& PinName, FString& OutError)
{
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			return Pin;
		}
	}

	// Build error with available pin names
	TArray<FString> PinNames;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin)
		{
			PinNames.Add(FString::Printf(TEXT("%s (%s)"),
				*Pin->PinName.ToString(),
				Pin->Direction == EGPD_Input ? TEXT("in") : TEXT("out")));
		}
	}

	OutError = FString::Printf(TEXT("Pin '%s' not found on '%s'. Available: %s"),
		*PinName,
		*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
		*FString::Join(PinNames, TEXT(", ")));
	return nullptr;
}

}  // namespace ClaireonAnimGraphTools_NodeInternal

// ============================================================================
// ClaireonAnimGraphTool_AddNode
// ============================================================================

FString ClaireonAnimGraphTool_AddNode::GetOperation() const { return TEXT("add_node"); }

FString ClaireonAnimGraphTool_AddNode::GetDescription() const
{
	return TEXT("Add an animation graph node by class name. Use node_properties to set properties "
		"BEFORE pin allocation (critical for SequencePlayer, BlendSpace, etc. where pins depend on the asset). "
		"Class name is fuzzy-matched (e.g., 'SequencePlayer' → AnimGraphNode_SequencePlayer).");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_AddNode::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("node_class"), TEXT("Class name of the node to add (e.g., AnimGraphNode_SequencePlayer, SequencePlayer, BlendSpacePlayer, StateMachine, TwoWayBlend)"), true);
	S.AddObject(TEXT("position"), TEXT("Position {x, y} for the new node"));
	S.AddBoolean(TEXT("auto_connect_from_cursor"), TEXT("Auto-connect new node's output pose to cursor node's input pose (default: false)"));
	S.AddObject(TEXT("node_properties"), TEXT("Properties to set on node BEFORE pin allocation (e.g., {\"Node.Sequence\": \"/Game/Path/To/Anim\"})"));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_AddNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();

	// Get node class name
	FString NodeClassName;
	if (!Arguments->TryGetStringField(TEXT("node_class"), NodeClassName))
	{
		return MakeErrorResult(TEXT("Missing required field: node_class"));
	}

	// Resolve class name — try UAnimGraphNode_Base first, then UK2Node for K2 nodes (PropertyAccess, etc.)
	ClaireonNameResolver::FNameResolveResult ResolveResult;
	UClass* NodeClass = ClaireonNameResolver::ResolveClassName(NodeClassName, UAnimGraphNode_Base::StaticClass(), ResolveResult);
	if (!NodeClass)
	{
		// Try UK2Node base (supports K2Node_PropertyAccess, K2Node_CallFunction, etc.)
		ClaireonNameResolver::FNameResolveResult K2ResolveResult;
		NodeClass = ClaireonNameResolver::ResolveClassName(NodeClassName, UK2Node::StaticClass(), K2ResolveResult);
		if (NodeClass)
		{
			ResolveResult = K2ResolveResult;
		}
		else
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to resolve node class '%s': %s"), *NodeClassName, *ResolveResult.Error));
		}
	}

	// Position
	FVector2D Position(0.0f, 0.0f);
	const TSharedPtr<FJsonObject>* PosObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("position"), PosObj) && PosObj && (*PosObj).IsValid())
	{
		(*PosObj)->TryGetNumberField(TEXT("x"), Position.X);
		(*PosObj)->TryGetNumberField(TEXT("y"), Position.Y);
	}
	else
	{
		// Default position near cursor
		Position = Data->Cursor.ViewportCenter + FVector2D(-200.0f, 0.0f);
	}

	// Create the node
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add AnimGraph Node")));
	AnimBP->Modify();
	Graph->Modify();

	UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, NodeClass);
	if (!NewNode)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create node of class: %s"), *NodeClass->GetName()));
	}

	// Apply node_properties BEFORE AllocateDefaultPins (critical for nodes whose pins depend on properties)
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	TArray<FString> Warnings;
	if (Arguments->TryGetObjectField(TEXT("node_properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
	{
		for (const auto& Pair : (*PropsObj)->Values)
		{
			FString Value;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(Value))
			{
				FString PropError;
				if (!ClaireonPropertyUtils::WritePropertyByPath(NewNode, FString(*Pair.Key), Value, PropError))
				{
					Warnings.Add(FString::Printf(TEXT("Failed to set property '%s': %s"), *Pair.Key, *PropError));
				}
			}
		}
	}

	// Standard node initialization
	NewNode->Rename(nullptr, Graph);
	Graph->AddNode(NewNode, true, false);
	NewNode->CreateNewGuid();
	NewNode->PostPlacedNewNode();
	NewNode->AllocateDefaultPins();
	NewNode->NodePosX = static_cast<int32>(Position.X);
	NewNode->NodePosY = static_cast<int32>(Position.Y);
	NewNode->SetFlags(RF_Transactional);

	if (!ResolveResult.ResolutionNote.IsEmpty())
	{
		Warnings.Add(ResolveResult.ResolutionNote);
	}

	// Auto-connect from cursor
	bool bAutoConnect = false;
	Arguments->TryGetBoolField(TEXT("auto_connect_from_cursor"), bAutoConnect);
	if (bAutoConnect && Data->Cursor.FocusedNodeGuid.IsValid())
	{
		UEdGraphNode* CursorNode = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, Data->Cursor.FocusedNodeGuid);
		if (CursorNode)
		{
			// Try to auto-wire: find compatible pose pins
			const UEdGraphSchema* Schema = Graph->GetSchema();
			if (Schema)
			{
				NewNode->AutowireNewNode(nullptr);

				// Try connecting first output pose of new node to first input pose of cursor node
				for (UEdGraphPin* OutputPin : NewNode->Pins)
				{
					if (!OutputPin || OutputPin->Direction != EGPD_Output) continue;
					for (UEdGraphPin* InputPin : CursorNode->Pins)
					{
						if (!InputPin || InputPin->Direction != EGPD_Input) continue;
						if (InputPin->LinkedTo.Num() > 0) continue; // Already connected
						FPinConnectionResponse Response = Schema->CanCreateConnection(OutputPin, InputPin);
						if (Response.Response != CONNECT_RESPONSE_DISALLOW)
						{
							Schema->TryCreateConnection(OutputPin, InputPin);
							Data->LastOperationAffectedNodes.Add(CursorNode->NodeGuid);
							goto AutoConnectDone;
						}
					}
				}
			}
		}
	}
AutoConnectDone:

	// Mark modified and notify
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	Graph->NotifyGraphChanged();

	// Update cursor to new node
	Data->Cursor.PushHistory(Data->Cursor.GraphName);
	Data->Cursor.FocusedNodeGuid = NewNode->NodeGuid;
	Data->Cursor.ViewportCenter = FVector2D(NewNode->NodePosX, NewNode->NodePosY);

	// Find first output pin for cursor
	for (UEdGraphPin* Pin : NewNode->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output)
		{
			Data->Cursor.FocusedPinName = Pin->PinName;
			Data->Cursor.FocusedPinDirection = EGPD_Output;
			break;
		}
	}

	Data->LastOperationAffectedNodes.Add(NewNode->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Added %s (GUID: %s)"),
		*NewNode->GetNodeTitle(ENodeTitleType::ListView).ToString(),
		*NewNode->NodeGuid.ToString());

	FToolResult Result = BuildStateResponse(SessionId, Data);
	Result.Warnings = Warnings;
	return Result;
}

// ============================================================================
// ClaireonAnimGraphTool_RemoveNode
// ============================================================================

FString ClaireonAnimGraphTool_RemoveNode::GetOperation() const { return TEXT("remove_node"); }

FString ClaireonAnimGraphTool_RemoveNode::GetDescription() const
{
    return TEXT("Remove an animation graph node by GUID. Breaks all pin connections first. Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_RemoveNode::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("node_guid"), TEXT("GUID of the node to remove"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_RemoveNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();

	FString NodeGuidStr;
	if (!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}

	FString FindError;
	UEdGraphNode* Node = ClaireonAnimGraphTools_NodeInternal::FindNodeByGuidOrTitle(Graph, NodeGuidStr, FindError);
	if (!Node)
	{
		return MakeErrorResult(FindError);
	}

	FString NodeTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();

	// Track connected nodes for change reporting
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (!Pin) continue;
		for (UEdGraphPin* Linked : Pin->LinkedTo)
		{
			if (Linked && Linked->GetOwningNode())
			{
				Data->LastOperationAffectedNodes.Add(Linked->GetOwningNode()->NodeGuid);
			}
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove AnimGraph Node")));
	AnimBP->Modify();
	Graph->Modify();

	// Break all pin connections
	Node->BreakAllNodeLinks();

	// Remove from graph
	Graph->RemoveNode(Node);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	Graph->NotifyGraphChanged();

	// Reset cursor if it was on the removed node
	if (Data->Cursor.FocusedNodeGuid == Node->NodeGuid)
	{
		Data->Cursor.FocusedNodeGuid = FGuid();
		Data->Cursor.FocusedPinName = NAME_None;
	}

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Removed node: %s"), *NodeTitle);
	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_MoveNode
// ============================================================================

FString ClaireonAnimGraphTool_MoveNode::GetOperation() const { return TEXT("move_node"); }

FString ClaireonAnimGraphTool_MoveNode::GetDescription() const
{
    return TEXT("Move an animation graph node to a new position in the open anim_graph session. Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_MoveNode::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("node_guid"), TEXT("GUID of the node to move"), true);
	S.AddObject(TEXT("position"), TEXT("New position {x, y} for the node"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_MoveNode::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UEdGraph* Graph = Data->CurrentGraph.Get();

	FString NodeGuidStr;
	if (!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
	{
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	}

	const TSharedPtr<FJsonObject>* PosObj = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("position"), PosObj) || !PosObj || !(*PosObj).IsValid())
	{
		return MakeErrorResult(TEXT("Missing required field: position"));
	}

	double X = 0, Y = 0;
	(*PosObj)->TryGetNumberField(TEXT("x"), X);
	(*PosObj)->TryGetNumberField(TEXT("y"), Y);

	FString FindError;
	UEdGraphNode* Node = ClaireonAnimGraphTools_NodeInternal::FindNodeByGuidOrTitle(Graph, NodeGuidStr, FindError);
	if (!Node)
	{
		return MakeErrorResult(FindError);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Move AnimGraph Node")));
	Node->Modify();
	Node->NodePosX = static_cast<int32>(X);
	Node->NodePosY = static_cast<int32>(Y);
	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Moved '%s' to (%d, %d)"),
		*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
		Node->NodePosX, Node->NodePosY);

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_SetNodeProperty
// ============================================================================

FString ClaireonAnimGraphTool_SetNodeProperty::GetOperation() const { return TEXT("set_node_property"); }

FString ClaireonAnimGraphTool_SetNodeProperty::GetDescription() const
{
	return TEXT("Set a property on an animation graph node. Works on both editor-level properties "
		"and inner FAnimNode runtime struct properties (via dot-path, e.g., 'Node.bLoopAnimation'). "
		"Calls ReconstructNode() if pin layout may change.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_SetNodeProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("node_guid"), TEXT("GUID of the target node"), true);
	S.AddString(TEXT("property_path"), TEXT("Property path to set (e.g., 'Node.Sequence', 'Node.bLoopAnimation')"), true);
	S.AddString(TEXT("value"), TEXT("Value to assign (string representation)"), true);
	S.AddBoolean(TEXT("reconstruct"), TEXT("Force ReconstructNode() after setting (default: auto-detect)"));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_SetNodeProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();

	FString NodeGuidStr, PropertyPath, Value;
	if (!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	if (!Arguments->TryGetStringField(TEXT("property_path"), PropertyPath))
		return MakeErrorResult(TEXT("Missing required field: property_path"));
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
		return MakeErrorResult(TEXT("Missing required field: value"));

	FString FindError;
	UEdGraphNode* Node = ClaireonAnimGraphTools_NodeInternal::FindNodeByGuidOrTitle(Graph, NodeGuidStr, FindError);
	if (!Node)
	{
		return MakeErrorResult(FindError);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set AnimGraph Node Property")));
	Node->Modify();

	FString PropError;
	bool bSuccess = ClaireonPropertyUtils::WritePropertyByPath(Node, PropertyPath, Value, PropError);
	if (!bSuccess)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to set '%s' = '%s': %s"), *PropertyPath, *Value, *PropError));
	}

	// Reconstruct node if requested or if the property might affect pins
	bool bReconstruct = false;
	if (!Arguments->TryGetBoolField(TEXT("reconstruct"), bReconstruct))
	{
		// Auto-detect: reconstruct if property path starts with "Node." (runtime struct property)
		bReconstruct = PropertyPath.StartsWith(TEXT("Node."));
	}

	if (bReconstruct)
	{
		Node->ReconstructNode();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	}
	else
	{
		FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	}

	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Set '%s' = '%s' on '%s'"),
		*PropertyPath, *Value,
		*Node->GetNodeTitle(ENodeTitleType::ListView).ToString());

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_ConnectPins
// ============================================================================

FString ClaireonAnimGraphTool_ConnectPins::GetOperation() const { return TEXT("connect_pins"); }

FString ClaireonAnimGraphTool_ConnectPins::GetDescription() const
{
	return TEXT("Connect two pins between animation graph nodes. Uses the animation graph schema "
		"(not K2 schema) for pose pin validation. Nodes can be identified by GUID or title.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_ConnectPins::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("source_node"), TEXT("Source node GUID or title"), true);
	S.AddString(TEXT("source_pin"), TEXT("Source pin name"), true);
	S.AddString(TEXT("target_node"), TEXT("Target node GUID or title"), true);
	S.AddString(TEXT("target_pin"), TEXT("Target pin name"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_ConnectPins::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();

	FString SourceNodeStr, SourcePinStr, TargetNodeStr, TargetPinStr;
	if (!Arguments->TryGetStringField(TEXT("source_node"), SourceNodeStr))
		return MakeErrorResult(TEXT("Missing required field: source_node"));
	if (!Arguments->TryGetStringField(TEXT("source_pin"), SourcePinStr))
		return MakeErrorResult(TEXT("Missing required field: source_pin"));
	if (!Arguments->TryGetStringField(TEXT("target_node"), TargetNodeStr))
		return MakeErrorResult(TEXT("Missing required field: target_node"));
	if (!Arguments->TryGetStringField(TEXT("target_pin"), TargetPinStr))
		return MakeErrorResult(TEXT("Missing required field: target_pin"));

	// Find nodes
	FString FindError;
	UEdGraphNode* SourceNode = ClaireonAnimGraphTools_NodeInternal::FindNodeByGuidOrTitle(Graph, SourceNodeStr, FindError);
	if (!SourceNode) return MakeErrorResult(FindError);

	UEdGraphNode* TargetNode = ClaireonAnimGraphTools_NodeInternal::FindNodeByGuidOrTitle(Graph, TargetNodeStr, FindError);
	if (!TargetNode) return MakeErrorResult(FindError);

	// Find pins
	UEdGraphPin* SourcePin = ClaireonAnimGraphTools_NodeInternal::FindPinByName(SourceNode, SourcePinStr, FindError);
	if (!SourcePin) return MakeErrorResult(FindError);

	UEdGraphPin* TargetPin = ClaireonAnimGraphTools_NodeInternal::FindPinByName(TargetNode, TargetPinStr, FindError);
	if (!TargetPin) return MakeErrorResult(FindError);

	// Use the animation graph schema for validation (NOT K2Schema)
	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		return MakeErrorResult(TEXT("Graph has no schema"));
	}

	FPinConnectionResponse Response = Schema->CanCreateConnection(SourcePin, TargetPin);
	if (Response.Response == CONNECT_RESPONSE_DISALLOW)
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot connect: %s"), *Response.Message.ToString()));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Connect AnimGraph Pins")));
	AnimBP->Modify();

	// Break existing connections if needed
	if (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A)
	{
		SourcePin->BreakAllPinLinks();
	}
	if (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B)
	{
		TargetPin->BreakAllPinLinks();
	}

	bool bConnected = Schema->TryCreateConnection(SourcePin, TargetPin);
	if (!bConnected)
	{
		return MakeErrorResult(TEXT("TryCreateConnection failed"));
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(SourceNode->NodeGuid);
	Data->LastOperationAffectedNodes.Add(TargetNode->NodeGuid);

	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Connected %s.%s → %s.%s"),
		*SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *SourcePinStr,
		*TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *TargetPinStr);

	return BuildStateResponse(SessionId, Data);
}

// ============================================================================
// ClaireonAnimGraphTool_DisconnectPin
// ============================================================================

FString ClaireonAnimGraphTool_DisconnectPin::GetOperation() const { return TEXT("disconnect_pin"); }

FString ClaireonAnimGraphTool_DisconnectPin::GetDescription() const
{
    return TEXT("Disconnect a pin on an animation graph node in the open anim_graph session. Optionally target a specific connection by providing target_node_guid. Session-mode tool: open via anim_graph_open first.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_DisconnectPin::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddString(TEXT("node_guid"), TEXT("GUID of the node whose pin to disconnect"), true);
	S.AddString(TEXT("pin_name"), TEXT("Name of the pin to disconnect"), true);
	S.AddString(TEXT("target_node_guid"), TEXT("Optional: GUID of the specific target node to disconnect from"));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_DisconnectPin::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();

	FString NodeGuidStr, PinName;
	if (!Arguments->TryGetStringField(TEXT("node_guid"), NodeGuidStr))
		return MakeErrorResult(TEXT("Missing required field: node_guid"));
	if (!Arguments->TryGetStringField(TEXT("pin_name"), PinName))
		return MakeErrorResult(TEXT("Missing required field: pin_name"));

	FString FindError;
	UEdGraphNode* Node = ClaireonAnimGraphTools_NodeInternal::FindNodeByGuidOrTitle(Graph, NodeGuidStr, FindError);
	if (!Node) return MakeErrorResult(FindError);

	UEdGraphPin* Pin = ClaireonAnimGraphTools_NodeInternal::FindPinByName(Node, PinName, FindError);
	if (!Pin) return MakeErrorResult(FindError);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Disconnect AnimGraph Pin")));
	AnimBP->Modify();

	// Track affected nodes before disconnecting
	for (UEdGraphPin* Linked : Pin->LinkedTo)
	{
		if (Linked && Linked->GetOwningNode())
		{
			Data->LastOperationAffectedNodes.Add(Linked->GetOwningNode()->NodeGuid);
		}
	}

	int32 BrokenCount = 0;
	FString TargetGuidStr;
	if (Arguments->TryGetStringField(TEXT("target_node_guid"), TargetGuidStr) && !TargetGuidStr.IsEmpty())
	{
		// Selective disconnect — only break link to the specified target
		FGuid TargetGuid;
		FGuid::Parse(TargetGuidStr, TargetGuid);

		for (int32 i = Pin->LinkedTo.Num() - 1; i >= 0; --i)
		{
			UEdGraphPin* Linked = Pin->LinkedTo[i];
			if (Linked && Linked->GetOwningNode() && Linked->GetOwningNode()->NodeGuid == TargetGuid)
			{
				Pin->BreakLinkTo(Linked);
				BrokenCount++;
			}
		}
	}
	else
	{
		// Break all links on this pin
		BrokenCount = Pin->LinkedTo.Num();
		Pin->BreakAllPinLinks();
	}

	FBlueprintEditorUtils::MarkBlueprintAsModified(AnimBP);
	Graph->NotifyGraphChanged();

	Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Disconnected %d link(s) from %s.%s"),
		BrokenCount,
		*Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
		*PinName);

	return BuildStateResponse(SessionId, Data);
}

#undef LOCTEXT_NAMESPACE
