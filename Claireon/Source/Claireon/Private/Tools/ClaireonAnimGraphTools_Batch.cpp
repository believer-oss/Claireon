// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimGraphTools_Batch.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Tools/ClaireonAnimGraphEditBase.h"
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonNameResolver.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLog.h"

#include "Animation/AnimBlueprint.h"
#include "AnimGraphNode_Base.h"
#include "K2Node.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "ScopedTransaction.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

#define LOCTEXT_NAMESPACE "ClaireonAnimGraphTools_Batch"

namespace ClaireonAnimGraphTools_BatchInternal
{

// ============================================================================
// Helper: Resolve a node reference (local ID, GUID, or title)
// ============================================================================

UEdGraphNode* ResolveNodeRef(
	const FString& Ref,
	const TMap<FString, UEdGraphNode*>& LocalIdMap,
	UEdGraph* Graph,
	FString& OutError)
{
	// Check local ID map first (new nodes created in this batch)
	if (UEdGraphNode* const* Found = LocalIdMap.Find(Ref))
	{
		return *Found;
	}

	// Try GUID
	FGuid ParsedGuid;
	if (FGuid::Parse(Ref, ParsedGuid) && ParsedGuid.IsValid())
	{
		UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, ParsedGuid);
		if (Node) return Node;
	}

	// Try title match
	TArray<UEdGraphNode*> Matches = ClaireonBlueprintHelpers::FindNodesByTitle(Graph, Ref);
	if (Matches.Num() == 1)
	{
		return Matches[0];
	}
	if (Matches.Num() > 1)
	{
		OutError = FString::Printf(TEXT("Multiple nodes match title '%s' — use GUID or local ID instead"), *Ref);
		return nullptr;
	}

	OutError = FString::Printf(TEXT("Node reference '%s' not found (checked: local IDs, GUIDs, titles)"), *Ref);
	return nullptr;
}

// ============================================================================
// Helper: Find pin by name on a node
// ============================================================================

UEdGraphPin* FindPinOnNode(UEdGraphNode* Node, const FString& PinName, FString& OutError)
{
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			return Pin;
		}
	}

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

}  // namespace ClaireonAnimGraphTools_BatchInternal

// ============================================================================
// ClaireonAnimGraphTool_ApplyDelta
// ============================================================================

FString ClaireonAnimGraphTool_ApplyDelta::GetOperation() const { return TEXT("apply_delta"); }

FString ClaireonAnimGraphTool_ApplyDelta::GetDescription() const
{
	return TEXT("Atomic batch graph construction and modification. Disconnects, removes, creates nodes, "
		"and connects pins in one call. New nodes are referenced by local 'id'; existing nodes by GUID or title. "
		"Execution order: disconnect → remove → create → connect. "
		"Returns full graph state with id_map showing local-id → GUID mappings.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_ApplyDelta::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID"), true);
	S.AddObject(TEXT("disconnect"), TEXT("Array of connections to break first. Each: {node (GUID/title), pin, target_node? (GUID/title for selective disconnect)}"));
	S.AddObject(TEXT("remove_nodes"), TEXT("Array of node GUIDs or titles to remove"));
	S.AddObject(TEXT("nodes"), TEXT("Array of nodes to create. Each: {id (local ref), class, position?: {x,y}, properties?: {path: value}}"));
	S.AddObject(TEXT("connections"), TEXT("Array of connections. Each: {from (local id/GUID/title), from_pin, to (local id/GUID/title), to_pin}"));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_ApplyDelta::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FAnimGraphEditToolData* Data = nullptr;
	FToolResult Error;
	if (!RequireSession(Arguments, SessionId, Data, Error)) return Error;

	UAnimBlueprint* AnimBP = Data->AnimBlueprint.Get();
	UEdGraph* Graph = Data->CurrentGraph.Get();
	if (!AnimBP || !Graph)
	{
		return MakeErrorResult(TEXT("AnimBP or Graph no longer valid"));
	}

	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		return MakeErrorResult(TEXT("Graph has no schema"));
	}

	TArray<FString> Warnings;

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Apply Graph (batch)")));
	AnimBP->Modify();
	Graph->Modify();

	// ========================================================================
	// Phase 1: Disconnect existing connections
	// ========================================================================

	const TArray<TSharedPtr<FJsonValue>>* DisconnectArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("disconnect"), DisconnectArray))
	{
		for (const TSharedPtr<FJsonValue>& Entry : *DisconnectArray)
		{
			const TSharedPtr<FJsonObject>* EntryObj = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(EntryObj) || !EntryObj) continue;

			FString NodeRef, PinName, TargetRef;
			if (!(*EntryObj)->TryGetStringField(TEXT("node"), NodeRef)) continue;
			if (!(*EntryObj)->TryGetStringField(TEXT("pin"), PinName)) continue;
			(*EntryObj)->TryGetStringField(TEXT("target_node"), TargetRef);

			TMap<FString, UEdGraphNode*> EmptyMap; // No local IDs yet
			FString FindError;
			UEdGraphNode* Node = ClaireonAnimGraphTools_BatchInternal::ResolveNodeRef(NodeRef, EmptyMap, Graph, FindError);
			if (!Node)
			{
				Warnings.Add(FString::Printf(TEXT("disconnect: %s"), *FindError));
				continue;
			}

			UEdGraphPin* Pin = ClaireonAnimGraphTools_BatchInternal::FindPinOnNode(Node, PinName, FindError);
			if (!Pin)
			{
				Warnings.Add(FString::Printf(TEXT("disconnect: %s"), *FindError));
				continue;
			}

			if (!TargetRef.IsEmpty())
			{
				// Selective disconnect
				UEdGraphNode* TargetNode = ClaireonAnimGraphTools_BatchInternal::ResolveNodeRef(TargetRef, EmptyMap, Graph, FindError);
				if (TargetNode)
				{
					for (int32 i = Pin->LinkedTo.Num() - 1; i >= 0; --i)
					{
						if (Pin->LinkedTo[i] && Pin->LinkedTo[i]->GetOwningNode() == TargetNode)
						{
							Data->LastOperationAffectedNodes.Add(TargetNode->NodeGuid);
							Pin->BreakLinkTo(Pin->LinkedTo[i]);
						}
					}
				}
			}
			else
			{
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked && Linked->GetOwningNode())
						Data->LastOperationAffectedNodes.Add(Linked->GetOwningNode()->NodeGuid);
				}
				Pin->BreakAllPinLinks();
			}
			Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
		}
	}

	// ========================================================================
	// Phase 2: Remove existing nodes
	// ========================================================================

	const TArray<TSharedPtr<FJsonValue>>* RemoveArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("remove_nodes"), RemoveArray))
	{
		TMap<FString, UEdGraphNode*> EmptyMap;
		for (const TSharedPtr<FJsonValue>& Entry : *RemoveArray)
		{
			FString NodeRef;
			if (!Entry.IsValid() || !Entry->TryGetString(NodeRef)) continue;

			FString FindError;
			UEdGraphNode* Node = ClaireonAnimGraphTools_BatchInternal::ResolveNodeRef(NodeRef, EmptyMap, Graph, FindError);
			if (!Node)
			{
				Warnings.Add(FString::Printf(TEXT("remove: %s"), *FindError));
				continue;
			}

			// Track connected nodes
			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked && Linked->GetOwningNode())
						Data->LastOperationAffectedNodes.Add(Linked->GetOwningNode()->NodeGuid);
				}
			}

			Node->BreakAllNodeLinks();
			Graph->RemoveNode(Node);
		}
	}

	// ========================================================================
	// Phase 3: Create new nodes
	// ========================================================================

	TMap<FString, UEdGraphNode*> LocalIdMap;
	TSharedPtr<FJsonObject> IdMapJson = MakeShared<FJsonObject>();

	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("nodes"), NodesArray))
	{
		for (const TSharedPtr<FJsonValue>& Entry : *NodesArray)
		{
			const TSharedPtr<FJsonObject>* NodeObj = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(NodeObj) || !NodeObj) continue;

			FString LocalId, ClassName;
			if (!(*NodeObj)->TryGetStringField(TEXT("id"), LocalId))
			{
				Warnings.Add(TEXT("Node entry missing 'id' field — skipped"));
				continue;
			}
			if (!(*NodeObj)->TryGetStringField(TEXT("class"), ClassName))
			{
				Warnings.Add(FString::Printf(TEXT("Node '%s' missing 'class' field — skipped"), *LocalId));
				continue;
			}

			// Resolve class — try UAnimGraphNode_Base first, then UK2Node for K2 nodes
			ClaireonNameResolver::FNameResolveResult ResolveResult;
			UClass* NodeClass = ClaireonNameResolver::ResolveClassName(ClassName, UAnimGraphNode_Base::StaticClass(), ResolveResult);
			if (!NodeClass)
			{
				ClaireonNameResolver::FNameResolveResult K2Result;
				NodeClass = ClaireonNameResolver::ResolveClassName(ClassName, UK2Node::StaticClass(), K2Result);
				if (NodeClass)
				{
					ResolveResult = K2Result;
				}
				else
				{
					return MakeErrorResult(FString::Printf(TEXT("Node '%s': failed to resolve class '%s': %s"), *LocalId, *ClassName, *ResolveResult.Error));
				}
			}
			if (!ResolveResult.ResolutionNote.IsEmpty())
			{
				Warnings.Add(ResolveResult.ResolutionNote);
			}

			// Position
			FVector2D Position(0.0f, 0.0f);
			const TSharedPtr<FJsonObject>* PosObj = nullptr;
			if ((*NodeObj)->TryGetObjectField(TEXT("position"), PosObj) && PosObj && (*PosObj).IsValid())
			{
				(*PosObj)->TryGetNumberField(TEXT("x"), Position.X);
				(*PosObj)->TryGetNumberField(TEXT("y"), Position.Y);
			}

			// Create node (supports both UAnimGraphNode_Base and UK2Node subclasses)
			UEdGraphNode* NewNode = NewObject<UEdGraphNode>(Graph, NodeClass);
			if (!NewNode)
			{
				return MakeErrorResult(FString::Printf(TEXT("Node '%s': failed to create %s"), *LocalId, *NodeClass->GetName()));
			}

			// Apply properties BEFORE AllocateDefaultPins
			const TSharedPtr<FJsonObject>* PropsObj = nullptr;
			if ((*NodeObj)->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
			{
				for (const auto& Pair : (*PropsObj)->Values)
				{
					FString Value;
					if (Pair.Value.IsValid() && Pair.Value->TryGetString(Value))
					{
						FString PropError;
						if (!ClaireonPropertyUtils::WritePropertyByPath(NewNode, Pair.Key, Value, PropError))
						{
							Warnings.Add(FString::Printf(TEXT("Node '%s': property '%s': %s"), *LocalId, *Pair.Key, *PropError));
						}
					}
				}
			}

			// Initialize node
			NewNode->Rename(nullptr, Graph);
			Graph->AddNode(NewNode, true, false);
			NewNode->CreateNewGuid();
			NewNode->PostPlacedNewNode();
			NewNode->AllocateDefaultPins();
			NewNode->NodePosX = static_cast<int32>(Position.X);
			NewNode->NodePosY = static_cast<int32>(Position.Y);
			NewNode->SetFlags(RF_Transactional);

			// Register in local ID map
			LocalIdMap.Add(LocalId, NewNode);
			IdMapJson->SetStringField(LocalId, NewNode->NodeGuid.ToString());
			Data->LastOperationAffectedNodes.Add(NewNode->NodeGuid);

			UE_LOG(LogClaireon, Log, TEXT("[ApplyGraph] Created '%s' (%s) → GUID %s"),
				*LocalId, *NewNode->GetNodeTitle(ENodeTitleType::ListView).ToString(), *NewNode->NodeGuid.ToString());
		}
	}

	// ========================================================================
	// Phase 4: Connect pins
	// ========================================================================

	const TArray<TSharedPtr<FJsonValue>>* ConnArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("connections"), ConnArray))
	{
		for (const TSharedPtr<FJsonValue>& Entry : *ConnArray)
		{
			const TSharedPtr<FJsonObject>* ConnObj = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(ConnObj) || !ConnObj) continue;

			FString FromRef, FromPinName, ToRef, ToPinName;
			if (!(*ConnObj)->TryGetStringField(TEXT("from"), FromRef) ||
				!(*ConnObj)->TryGetStringField(TEXT("from_pin"), FromPinName) ||
				!(*ConnObj)->TryGetStringField(TEXT("to"), ToRef) ||
				!(*ConnObj)->TryGetStringField(TEXT("to_pin"), ToPinName))
			{
				Warnings.Add(TEXT("Connection entry missing from/from_pin/to/to_pin — skipped"));
				continue;
			}

			FString FindError;
			UEdGraphNode* FromNode = ClaireonAnimGraphTools_BatchInternal::ResolveNodeRef(FromRef, LocalIdMap, Graph, FindError);
			if (!FromNode)
			{
				return MakeErrorResult(FString::Printf(TEXT("Connection from '%s': %s"), *FromRef, *FindError));
			}

			UEdGraphNode* ToNode = ClaireonAnimGraphTools_BatchInternal::ResolveNodeRef(ToRef, LocalIdMap, Graph, FindError);
			if (!ToNode)
			{
				return MakeErrorResult(FString::Printf(TEXT("Connection to '%s': %s"), *ToRef, *FindError));
			}

			UEdGraphPin* FromPin = ClaireonAnimGraphTools_BatchInternal::FindPinOnNode(FromNode, FromPinName, FindError);
			if (!FromPin)
			{
				return MakeErrorResult(FString::Printf(TEXT("Connection %s.%s: %s"), *FromRef, *FromPinName, *FindError));
			}

			UEdGraphPin* ToPin = ClaireonAnimGraphTools_BatchInternal::FindPinOnNode(ToNode, ToPinName, FindError);
			if (!ToPin)
			{
				return MakeErrorResult(FString::Printf(TEXT("Connection %s.%s: %s"), *ToRef, *ToPinName, *FindError));
			}

			FPinConnectionResponse Response = Schema->CanCreateConnection(FromPin, ToPin);
			if (Response.Response == CONNECT_RESPONSE_DISALLOW)
			{
				return MakeErrorResult(FString::Printf(TEXT("Cannot connect %s.%s → %s.%s: %s"),
					*FromRef, *FromPinName, *ToRef, *ToPinName, *Response.Message.ToString()));
			}

			if (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A)
				FromPin->BreakAllPinLinks();
			if (Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B)
				ToPin->BreakAllPinLinks();

			bool bConnected = Schema->TryCreateConnection(FromPin, ToPin);
			if (!bConnected)
			{
				return MakeErrorResult(FString::Printf(TEXT("TryCreateConnection failed: %s.%s → %s.%s"),
					*FromRef, *FromPinName, *ToRef, *ToPinName));
			}

			Data->LastOperationAffectedNodes.Add(FromNode->NodeGuid);
			Data->LastOperationAffectedNodes.Add(ToNode->NodeGuid);
		}
	}

	// ========================================================================
	// Phase 5: Finalize
	// ========================================================================

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	Graph->NotifyGraphChanged();

	// Force full response to show all nodes
	Data->ResponseMode = TEXT("full");

	int32 NodesCreated = LocalIdMap.Num();
	Data->Cursor.LastOperationStatus = FString::Printf(TEXT("Applied graph: %d nodes created, %d connections made"),
		NodesCreated, ConnArray ? ConnArray->Num() : 0);

	FToolResult Result = BuildStateResponse(SessionId, Data);

	// Inject id_map into the response
	if (Result.Data.IsValid() && IdMapJson->Values.Num() > 0)
	{
		Result.Data->SetObjectField(TEXT("id_map"), IdMapJson);
	}

	Result.Warnings = Warnings;
	return Result;
}

#undef LOCTEXT_NAMESPACE
