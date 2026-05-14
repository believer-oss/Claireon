// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ApplyBlueprintGraph.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "ClaireonBlueprintNodeFactory.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLog.h"

#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"

#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

#define LOCTEXT_NAMESPACE "ClaireonTool_ApplyBlueprintGraph"

// ============================================================================
// Helpers — mirror the anim batch tool's node/pin resolution helpers
// ============================================================================

namespace
{
	UEdGraphNode* ResolveNodeRef(
		const FString& Ref,
		const TMap<FString, UEdGraphNode*>& LocalIdMap,
		UEdGraph* Graph,
		FString& OutError)
	{
		if (Ref.IsEmpty())
		{
			OutError = TEXT("Empty node reference");
			return nullptr;
		}

		if (UEdGraphNode* const* Found = LocalIdMap.Find(Ref))
		{
			return *Found;
		}

		FGuid ParsedGuid;
		if (FGuid::Parse(Ref, ParsedGuid) && ParsedGuid.IsValid())
		{
			if (UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, ParsedGuid))
			{
				return Node;
			}
		}

		TArray<UEdGraphNode*> Matches = ClaireonBlueprintHelpers::FindNodesByTitle(Graph, Ref);
		if (Matches.Num() == 1) return Matches[0];
		if (Matches.Num() > 1)
		{
			OutError = FString::Printf(TEXT("Multiple nodes match title '%s' — use GUID or local ID"), *Ref);
			return nullptr;
		}

		OutError = FString::Printf(TEXT("Node reference '%s' not found (checked local IDs, GUID, title)"), *Ref);
		return nullptr;
	}

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
}

// ============================================================================
// ClaireonTool_ApplyBlueprintGraph
// ============================================================================

FString ClaireonTool_ApplyBlueprintGraph::GetOperation() const { return TEXT("apply_graph"); }

FString ClaireonTool_ApplyBlueprintGraph::GetDescription() const
{
	return TEXT("Atomic batch K2 Blueprint graph construction and modification. Disconnects, removes, "
		"creates nodes, and connects pins in one transactional call. New nodes are referenced by local "
		"'id'; existing nodes by GUID or title. Execution order: disconnect → remove → create → connect. "
		"Uses the same session opened by blueprint_edit_graph. Returns full graph state with "
		"id_map showing local-id → GUID mappings. Counterpart to animgraph_apply_graph.");
}

TSharedPtr<FJsonObject> ClaireonTool_ApplyBlueprintGraph::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID from a prior blueprint_edit_graph 'open' or 'create'"), true);
	S.AddObject(TEXT("disconnect"), TEXT("Array of connections to break first. Each: {node (GUID/title/local_id), pin, target_node? (for selective disconnect)}"));
	S.AddObject(TEXT("remove_nodes"), TEXT("Array of node GUIDs or titles to remove"));
	S.AddObject(TEXT("nodes"), TEXT("Array of nodes to create. Each: {id (local ref), node_type, position?: {x,y}, ...typed params (function_name, struct_type, ...), node_properties?: {}, num_extra_pins?: int}"));
	S.AddObject(TEXT("connections"), TEXT("Array of connections. Each: {from (local id/GUID/title), from_pin, to (local id/GUID/title), to_pin}"));
	return S.Build();
}

FToolResult ClaireonTool_ApplyBlueprintGraph::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// -------- Session lookup (piggybacks on blueprint_edit_graph's session map) --------
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: session_id"));
	}

	FBlueprintEditToolData* Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
	if (!Data)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Session '%s' not found. Open a blueprint session first via blueprint_edit_graph with operation='open' or 'create'."),
			*SessionId));
	}

	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();
	if (!Blueprint || !Graph)
	{
		return MakeErrorResult(TEXT("Blueprint or Graph no longer valid for this session"));
	}

	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!Schema)
	{
		return MakeErrorResult(TEXT("Graph has no schema"));
	}

	TArray<FString> Warnings;

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Apply Blueprint Graph (batch)")));
	Blueprint->Modify();
	Graph->Modify();

	Data->LastOperationAffectedNodes.Reset();
	TArray<UEdGraphNode*> CreatedNodesThisCall;

	// H1: transaction rollback helper. Every error-return after this point must
	// go through CancelAndError so the FScopedTransaction destructor discards the
	// partial work instead of committing half-created nodes with no connections.
	auto CancelAndError = [&](const FString& Msg)
	{
		// Safety net: if the transaction did not record node adds for any reason,
		// remove them explicitly while the nodes are still attached to the graph.
		// With the RF_Transactional fix in ClaireonBlueprintNodeFactory this is a
		// no-op (the Contains guard fails after Transaction.Cancel detaches them),
		// but it keeps apply_graph atomic if the factory ever regresses.
		for (UEdGraphNode* Node : CreatedNodesThisCall)
		{
			if (Node && IsValid(Node) && Graph->Nodes.Contains(Node))
			{
				Data->LastOperationAffectedNodes.Remove(Node->NodeGuid);
				Graph->RemoveNode(Node);
			}
		}
		Transaction.Cancel();
		return MakeErrorResult(Msg);
	};

	// ========================================================================
	// Phase 1: Disconnect
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

			TMap<FString, UEdGraphNode*> EmptyMap;
			FString FindErr;
			UEdGraphNode* Node = ResolveNodeRef(NodeRef, EmptyMap, Graph, FindErr);
			if (!Node)
			{
				Warnings.Add(FString::Printf(TEXT("disconnect: %s"), *FindErr));
				continue;
			}

			UEdGraphPin* Pin = FindPinOnNode(Node, PinName, FindErr);
			if (!Pin)
			{
				Warnings.Add(FString::Printf(TEXT("disconnect: %s"), *FindErr));
				continue;
			}

			if (!TargetRef.IsEmpty())
			{
				UEdGraphNode* Target = ResolveNodeRef(TargetRef, EmptyMap, Graph, FindErr);
				if (Target)
				{
					for (int32 i = Pin->LinkedTo.Num() - 1; i >= 0; --i)
					{
						if (Pin->LinkedTo[i] && Pin->LinkedTo[i]->GetOwningNode() == Target)
						{
							Data->LastOperationAffectedNodes.Add(Target->NodeGuid);
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
	// Phase 2: Remove
	// ========================================================================
	const TArray<TSharedPtr<FJsonValue>>* RemoveArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("remove_nodes"), RemoveArray))
	{
		TMap<FString, UEdGraphNode*> EmptyMap;
		for (const TSharedPtr<FJsonValue>& Entry : *RemoveArray)
		{
			FString Ref;
			if (!Entry.IsValid() || !Entry->TryGetString(Ref)) continue;

			FString FindErr;
			UEdGraphNode* Node = ResolveNodeRef(Ref, EmptyMap, Graph, FindErr);
			if (!Node)
			{
				Warnings.Add(FString::Printf(TEXT("remove: %s"), *FindErr));
				continue;
			}

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
	// Phase 3: Create
	// ========================================================================
	TMap<FString, UEdGraphNode*> LocalIdMap;
	TSharedPtr<FJsonObject> IdMapJson = MakeShared<FJsonObject>();

	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("nodes"), NodesArray))
	{
		int32 NodeIdx = 0;
		for (const TSharedPtr<FJsonValue>& Entry : *NodesArray)
		{
			const TSharedPtr<FJsonObject>* NodeObj = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(NodeObj) || !NodeObj) { ++NodeIdx; continue; }

			FString LocalId;
			if (!(*NodeObj)->TryGetStringField(TEXT("id"), LocalId))
			{
				Warnings.Add(FString::Printf(TEXT("nodes[%d]: missing 'id' — skipped"), NodeIdx));
				++NodeIdx;
				continue;
			}

			FVector2D Position(static_cast<float>(NodeIdx) * 300.0f, 0.0f);
			const TSharedPtr<FJsonObject>* PosObj = nullptr;
			if ((*NodeObj)->TryGetObjectField(TEXT("position"), PosObj) && PosObj && (*PosObj).IsValid())
			{
				double X = 0.0, Y = 0.0;
				(*PosObj)->TryGetNumberField(TEXT("x"), X);
				(*PosObj)->TryGetNumberField(TEXT("y"), Y);
				Position = FVector2D(X, Y);
			}

			// Accept animgraph-style {class, properties} in addition to typed-param {node_type, ...}
			// by translating to the existing Generic branch in ClaireonBlueprintNodeFactory.
			// Enables round-trip from get_graph's apply_spec output without caller-side translation.
			TSharedPtr<FJsonObject> NodeParams = *NodeObj;
			if (!NodeParams->HasField(TEXT("node_type")))
			{
				FString ClassName;
				if (NodeParams->TryGetStringField(TEXT("class"), ClassName) && !ClassName.IsEmpty())
				{
					TSharedPtr<FJsonObject> Routed = MakeShared<FJsonObject>();
					// Copy everything the caller provided
					for (const auto& P : NodeParams->Values) Routed->SetField(P.Key, P.Value);
					// Set the Generic branch shape
					Routed->SetStringField(TEXT("node_type"), TEXT("Generic"));
					Routed->SetStringField(TEXT("class_name"), ClassName);
					// Translate `properties` → `node_properties` if the caller used the animgraph key.
					// Factory's Generic branch consumes `node_properties`; leave `properties` alone
					// in case a future case-specific branch reads it.
					const TSharedPtr<FJsonObject>* PropsObj = nullptr;
					if (!NodeParams->HasField(TEXT("node_properties"))
						&& NodeParams->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj && (*PropsObj).IsValid())
					{
						Routed->SetObjectField(TEXT("node_properties"), *PropsObj);
					}
					NodeParams = Routed;
				}
			}

			ClaireonBlueprintNodeFactory::FCreateResult R = ClaireonBlueprintNodeFactory::CreateNode(
				Blueprint, Graph, NodeParams, Position);

			if (!R.IsOk())
			{
				return CancelAndError(FString::Printf(TEXT("nodes[%d] '%s': %s"), NodeIdx, *LocalId, *R.Error));
			}
			for (const FString& W : R.Warnings)
			{
				Warnings.Add(FString::Printf(TEXT("nodes[%d] '%s': %s"), NodeIdx, *LocalId, *W));
			}

			LocalIdMap.Add(LocalId, R.Node);
			IdMapJson->SetStringField(LocalId, R.Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
			Data->LastOperationAffectedNodes.Add(R.Node->NodeGuid);
			CreatedNodesThisCall.Add(R.Node);

			UE_LOG(LogClaireon, Log, TEXT("[BP ApplyGraph] Created '%s' (%s) → GUID %s"),
				*LocalId,
				*R.Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
				*R.Node->NodeGuid.ToString());

			++NodeIdx;
		}
	}

	// ========================================================================
	// Phase 4: Connect
	// ========================================================================
	int32 ConnectionsMade = 0;
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
				Warnings.Add(TEXT("connection entry missing from/from_pin/to/to_pin — skipped"));
				continue;
			}

			FString FindErr;
			UEdGraphNode* FromNode = ResolveNodeRef(FromRef, LocalIdMap, Graph, FindErr);
			if (!FromNode)
			{
				return CancelAndError(FString::Printf(TEXT("connection from '%s': %s"), *FromRef, *FindErr));
			}
			UEdGraphNode* ToNode = ResolveNodeRef(ToRef, LocalIdMap, Graph, FindErr);
			if (!ToNode)
			{
				return CancelAndError(FString::Printf(TEXT("connection to '%s': %s"), *ToRef, *FindErr));
			}

			UEdGraphPin* FromPin = FindPinOnNode(FromNode, FromPinName, FindErr);
			if (!FromPin)
			{
				return CancelAndError(FString::Printf(TEXT("connection %s.%s: %s"), *FromRef, *FromPinName, *FindErr));
			}
			UEdGraphPin* ToPin = FindPinOnNode(ToNode, ToPinName, FindErr);
			if (!ToPin)
			{
				return CancelAndError(FString::Printf(TEXT("connection %s.%s: %s"), *ToRef, *ToPinName, *FindErr));
			}

			const FPinConnectionResponse Resp = Schema->CanCreateConnection(FromPin, ToPin);
			if (Resp.Response == CONNECT_RESPONSE_DISALLOW)
			{
				return CancelAndError(FString::Printf(TEXT("Cannot connect %s.%s → %s.%s: %s"),
					*FromRef, *FromPinName, *ToRef, *ToPinName, *Resp.Message.ToString()));
			}

			if (Resp.Response == CONNECT_RESPONSE_BREAK_OTHERS_A) FromPin->BreakAllPinLinks();
			if (Resp.Response == CONNECT_RESPONSE_BREAK_OTHERS_B) ToPin->BreakAllPinLinks();

			if (!Schema->TryCreateConnection(FromPin, ToPin))
			{
				return CancelAndError(FString::Printf(TEXT("TryCreateConnection failed: %s.%s → %s.%s"),
					*FromRef, *FromPinName, *ToRef, *ToPinName));
			}

			Data->LastOperationAffectedNodes.Add(FromNode->NodeGuid);
			Data->LastOperationAffectedNodes.Add(ToNode->NodeGuid);
			++ConnectionsMade;
		}
	}

	// ========================================================================
	// Phase 5: Finalize
	// ========================================================================
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	Graph->NotifyGraphChanged();

	// Force full response (sidesteps per-op snapshot path in BuildStateResponse)
	Data->ResponseMode = TEXT("full");
	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Applied BP graph: %d nodes created, %d connection(s) made"),
		LocalIdMap.Num(), ConnectionsMade);

	// Build response by reusing blueprint_edit_graph's response builder — we go through
	// our own tool's data pointer to reach the private BuildStateResponse. Since we don't
	// subclass, we replicate the structure here minimally: put the key info in Data.
	TSharedPtr<FJsonObject> OutData = MakeShared<FJsonObject>();
	OutData->SetStringField(TEXT("session_id"), SessionId);
	OutData->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	OutData->SetStringField(TEXT("graph"), Graph->GetName());
	OutData->SetNumberField(TEXT("nodes_created"), LocalIdMap.Num());
	OutData->SetNumberField(TEXT("connections_made"), ConnectionsMade);
	OutData->SetStringField(TEXT("status"), Data->Cursor.LastOperationStatus);
	if (IdMapJson->Values.Num() > 0)
	{
		OutData->SetObjectField(TEXT("id_map"), IdMapJson);
	}

	FToolResult Result = MakeSuccessResult(OutData, Data->Cursor.LastOperationStatus);
	Result.Warnings = Warnings;
	return Result;
}

#undef LOCTEXT_NAMESPACE
