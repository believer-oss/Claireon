// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ApplyBlueprintDelta.h"
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

#define LOCTEXT_NAMESPACE "ClaireonTool_ApplyBlueprintDelta"

// ============================================================================
// Helpers — mirror the anim batch tool's node/pin resolution helpers
// ============================================================================

namespace ClaireonTool_ApplyBlueprintDelta_Private
{
	// File-local discriminator prefix (BPApplyDelta_) on every anon-namespace
	// helper: anon namespaces are not isolation under unity batching, and
	// ResolveNodeRef/FindPinOnNode also exist in ClaireonAnimGraphTools_Batch.cpp.

	// True if Ref is a plausible GUID prefix: after stripping hyphens it is
	// 8..32 hex characters. Output is the uppercased hyphen-free hex prefix,
	// comparable against FGuid::ToString(EGuidFormats::Digits).
	bool BPApplyDelta_TryNormalizeGuidPrefix(const FString& Ref, FString& OutHexPrefix)
	{
		FString Stripped = Ref.Replace(TEXT("-"), TEXT(""));
		if (Stripped.Len() < 8 || Stripped.Len() > 32)
		{
			return false;
		}
		for (int32 I = 0; I < Stripped.Len(); ++I)
		{
			if (!FChar::IsHexDigit(Stripped[I]))
			{
				return false;
			}
		}
		OutHexPrefix = Stripped.ToUpper();
		return true;
	}

	UEdGraphNode* BPApplyDelta_ResolveNodeRef(
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
			if (UEdGraphNode* Node = ClaireonBlueprintHelpers::FindNodeByGuid(Graph, ParsedGuid); IsValid(Node))
			{
				return Node;
			}
		}

		// GUID prefix: >= 8 hex chars (hyphens ignored) matching exactly one
		// node's GUID. Ambiguity is a hard error naming every candidate; zero
		// matches fall through to title matching (a hex-looking title is legal).
		FString HexPrefix;
		if (BPApplyDelta_TryNormalizeGuidPrefix(Ref, HexPrefix))
		{
			TArray<UEdGraphNode*> PrefixMatches;
			for (UEdGraphNode* Node : Graph->Nodes)
			{
				if (IsValid(Node) && Node->NodeGuid.ToString(EGuidFormats::Digits).StartsWith(HexPrefix, ESearchCase::IgnoreCase))
				{
					PrefixMatches.Add(Node);
				}
			}
			if (PrefixMatches.Num() == 1)
			{
				return PrefixMatches[0];
			}
			if (PrefixMatches.Num() > 1)
			{
				// Shares the formatter with the title path and with ResolveNodeGuidString:
				// one ambiguity sentence, and candidates carry titles rather than bare GUIDs.
				OutError = ClaireonBlueprintHelpers::FormatAmbiguousNodeMatch(
					Graph, TEXT("GUID prefix"), Ref, PrefixMatches,
					TEXT("Provide more characters or the full GUID."));
				return nullptr;
			}
		}

		TArray<UEdGraphNode*> Matches = ClaireonBlueprintHelpers::FindNodesByTitle(Graph, Ref);
		if (Matches.Num() == 1) return Matches[0];
		if (Matches.Num() > 1)
		{
			OutError = ClaireonBlueprintHelpers::FormatTitleMatchFailure(
				Graph, Ref, Matches, TEXT("a GUID or local ID"));
			return nullptr;
		}

		// Keep the local-ID/GUID-prefix context in the sentence -- the generic title-only
		// message would lose it -- and append the title suggestions when there are any.
		const FString Suggestions = ClaireonBlueprintHelpers::FormatTitleSuggestions(
			Graph, Ref, TEXT("a GUID or local ID"));
		OutError = FString::Printf(
			TEXT("Node reference '%s' not found (checked local IDs, GUID, GUID prefix, title)."), *Ref);
		if (!Suggestions.IsEmpty())
		{
			OutError += TEXT(" ") + Suggestions;
		}
		return nullptr;
	}

	UEdGraphPin* BPApplyDelta_FindPinOnNode(UEdGraphNode* Node, const FString& PinName, FString& OutError)
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
using namespace ClaireonTool_ApplyBlueprintDelta_Private;

// ============================================================================
// ClaireonTool_ApplyBlueprintDelta
// ============================================================================

FString ClaireonTool_ApplyBlueprintDelta::GetOperation() const { return TEXT("apply_delta"); }

FString ClaireonTool_ApplyBlueprintDelta::GetDescription() const
{
	return TEXT("Apply a batch of K2 graph edits in one transactional call: disconnects, node removals, node creations, "
		"pin defaults, and connections, in that order. New nodes are referenced by local 'id', existing nodes by "
		"GUID or title. Returns full graph state with id_map of local-id -> GUID. Session-mode tool: pass "
		"session_id from bp_open/bp_create, or asset_path to auto-open.");
}

TSharedPtr<FJsonObject> ClaireonTool_ApplyBlueprintDelta::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("session_id"), TEXT("Session ID from a prior bp_open or bp_create call (or pass asset_path to auto-open)"), false);
	S.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id; auto-opens a session)"), false);
	S.AddArray(TEXT("disconnect"), TEXT("Array of connections to break first. Each: {node (GUID/unique GUID prefix of >= 8 hex chars/title/local_id), pin, target_node? (for selective disconnect)}"));
	S.AddArray(TEXT("remove_nodes"), TEXT("Array of node GUIDs (full, or a unique prefix of >= 8 hex chars) or titles to remove"));
	S.AddArray(TEXT("nodes"), TEXT("Array of nodes to create. Each: {id (local ref), node_type, position?: {x,y}, ...typed params (function_name, struct_type, ...), node_properties?: {}, num_extra_pins?: int}"));
	S.AddArray(TEXT("pin_defaults"), TEXT("Array of pin default values to write after nodes are created and before connections are made. Each: {node (local id/GUID/unique GUID prefix of >= 8 hex chars/title), pin, value}. Input pins only, and the pin must be unconnected. Any failing entry rolls back the WHOLE batch, like every other op here."));
	S.AddArray(TEXT("connections"), TEXT("Array of connections. Each: {from (local id/GUID/unique GUID prefix of >= 8 hex chars/title), from_pin, to (same forms), to_pin}. Ambiguous GUID prefixes error naming every candidate. On abort the whole batch rolls back and the error's data carries a structured report: node_specs (per-spec status), id_map (rolled-back local-id -> GUID), and failing_connection_index. Deprecated dict form {connections:[...]} is accepted with a warning."));
	return S.Build();
}

FToolResult ClaireonTool_ApplyBlueprintDelta::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Shared session prologue: session_id lookup, asset_path auto-open, nested-params
	// unwrap, response_mode/suppress_output. Same entry shape as every sibling bp_*
	// tool, and what makes this tool's long-standing asset_path claim true.
	TSharedPtr<FJsonObject> Params;
	FString SessionId;
	FBlueprintEditToolData* Data = nullptr;
	FToolResult SessionError;
	if (!BeginSessionOp(Arguments, TEXT("apply_delta"), Params, SessionId, Data, SessionError))
	{
		return SessionError;
	}
	// Read the op arrays off the unwrapped params, not the raw Arguments, so the
	// legacy nested-"params" envelope keeps working.
	const TSharedPtr<FJsonObject>& Ops = Params;

	UBlueprint* Blueprint = Data->Blueprint.Get();
	UEdGraph* Graph = Data->Graph.Get();
	if (!IsValid(Blueprint) || !IsValid(Graph))
	{
		return MakeErrorResult(TEXT("Blueprint or Graph no longer valid for this session"));
	}

	const UEdGraphSchema* Schema = Graph->GetSchema();
	if (!IsValid(Schema))
	{
		return MakeErrorResult(TEXT("Graph has no schema"));
	}

	TArray<FString> Warnings;

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Apply Blueprint Graph (batch)")));
	Blueprint->Modify();
	Graph->Modify();

	Data->LastOperationAffectedNodes.Reset();
	TArray<UEdGraphNode*> CreatedNodesThisCall;

	// Declared ahead of CancelAndError so the abort report can include them.
	// LocalIdMap/IdMapJson are populated in Phase 3; NodeSpecReport carries one
	// status object per node-spec entry; FailingConnectionIndex is set by the
	// Phase 4 error paths before aborting.
	TMap<FString, UEdGraphNode*> LocalIdMap;
	TSharedPtr<FJsonObject> IdMapJson = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> NodeSpecReport;
	int32 FailingConnectionIndex = INDEX_NONE;

	// transaction rollback helper. Every error-return after this point must
	// go through CancelAndError so the FScopedTransaction destructor discards the
	// partial work instead of committing half-created nodes with no connections.
	// The returned error carries a structured report in Data so callers can see
	// which specs were fine: node_specs (per-spec status, created entries marked
	// rolled back), id_map (local-id -> GUID of the rolled-back nodes), and
	// failing_connection_index when the failure was in the connections phase.
	auto CancelAndError = [&](const FString& Msg)
	{
		// Safety net: if the transaction did not record node adds for any reason,
		// remove them explicitly while the nodes are still attached to the graph.
		// With the RF_Transactional fix in ClaireonBlueprintNodeFactory this is a
		// no-op (the Contains guard fails after Transaction.Cancel detaches them),
		// but it keeps apply_delta atomic if the factory ever regresses.
		for (UEdGraphNode* Node : CreatedNodesThisCall)
		{
			if (IsValid(Node) && IsValid(Node) && Graph->Nodes.Contains(Node))
			{
				Data->LastOperationAffectedNodes.Remove(Node->NodeGuid);
				// Break links first so a connection made to a PRE-EXISTING node
				// earlier in this batch does not leave a dangling LinkedTo entry
				// on the surviving node after removal.
				Node->BreakAllNodeLinks();
				Graph->RemoveNode(Node);
			}
		}
		Transaction.Cancel();

		// Mark every successfully created spec as rolled back before attaching
		// the report -- the nodes no longer exist in the graph.
		for (const TSharedPtr<FJsonValue>& SpecVal : NodeSpecReport)
		{
			const TSharedPtr<FJsonObject>* SpecObj = nullptr;
			if (SpecVal.IsValid() && SpecVal->TryGetObject(SpecObj) && SpecObj && (*SpecObj).IsValid())
			{
				FString SpecStatus;
				if ((*SpecObj)->TryGetStringField(TEXT("status"), SpecStatus) && SpecStatus == TEXT("created"))
				{
					(*SpecObj)->SetStringField(TEXT("status"), TEXT("created_rolled_back"));
				}
			}
		}

		TSharedPtr<FJsonObject> AbortReport = MakeShared<FJsonObject>();
		AbortReport->SetArrayField(TEXT("node_specs"), NodeSpecReport);
		AbortReport->SetObjectField(TEXT("id_map"), IdMapJson);
		if (FailingConnectionIndex != INDEX_NONE)
		{
			AbortReport->SetNumberField(TEXT("failing_connection_index"), FailingConnectionIndex);
		}

		FToolResult ErrResult = MakeErrorResult(Msg);
		ErrResult.Data = AbortReport;
		ErrResult.Warnings = Warnings;
		return ErrResult;
	};

	// ========================================================================
	// Phase 1: Disconnect
	// ========================================================================
	const TArray<TSharedPtr<FJsonValue>>* DisconnectArray = nullptr;
	if (Ops->TryGetArrayField(TEXT("disconnect"), DisconnectArray))
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
			UEdGraphNode* Node = BPApplyDelta_ResolveNodeRef(NodeRef, EmptyMap, Graph, FindErr);
			if (!IsValid(Node))
			{
				Warnings.Add(FString::Printf(TEXT("disconnect: %s"), *FindErr));
				continue;
			}

			UEdGraphPin* Pin = BPApplyDelta_FindPinOnNode(Node, PinName, FindErr);
			if (!Pin)
			{
				Warnings.Add(FString::Printf(TEXT("disconnect: %s"), *FindErr));
				continue;
			}

			if (!TargetRef.IsEmpty())
			{
				UEdGraphNode* Target = BPApplyDelta_ResolveNodeRef(TargetRef, EmptyMap, Graph, FindErr);
				if (IsValid(Target))
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
					if (Linked && IsValid(Linked->GetOwningNode()))
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
	if (Ops->TryGetArrayField(TEXT("remove_nodes"), RemoveArray))
	{
		TMap<FString, UEdGraphNode*> EmptyMap;
		for (const TSharedPtr<FJsonValue>& Entry : *RemoveArray)
		{
			FString Ref;
			if (!Entry.IsValid() || !Entry->TryGetString(Ref)) continue;

			FString FindErr;
			UEdGraphNode* Node = BPApplyDelta_ResolveNodeRef(Ref, EmptyMap, Graph, FindErr);
			if (!IsValid(Node))
			{
				Warnings.Add(FString::Printf(TEXT("remove: %s"), *FindErr));
				continue;
			}

			for (UEdGraphPin* Pin : Node->Pins)
			{
				if (!Pin) continue;
				for (UEdGraphPin* Linked : Pin->LinkedTo)
				{
					if (Linked && IsValid(Linked->GetOwningNode()))
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
	// LocalIdMap / IdMapJson / NodeSpecReport are declared before CancelAndError
	// (above) so the abort report can include them.
	const TArray<TSharedPtr<FJsonValue>>* NodesArray = nullptr;
	if (Ops->TryGetArrayField(TEXT("nodes"), NodesArray))
	{
		int32 NodeIdx = 0;
		for (const TSharedPtr<FJsonValue>& Entry : *NodesArray)
		{
			const TSharedPtr<FJsonObject>* NodeObj = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(NodeObj) || !NodeObj)
			{
				TSharedPtr<FJsonObject> SpecStatus = MakeShared<FJsonObject>();
				SpecStatus->SetNumberField(TEXT("index"), NodeIdx);
				SpecStatus->SetStringField(TEXT("status"), TEXT("skipped_not_an_object"));
				NodeSpecReport.Add(MakeShared<FJsonValueObject>(SpecStatus));
				++NodeIdx;
				continue;
			}

			FString LocalId;
			if (!(*NodeObj)->TryGetStringField(TEXT("id"), LocalId))
			{
				Warnings.Add(FString::Printf(TEXT("nodes[%d]: missing 'id' — skipped"), NodeIdx));
				TSharedPtr<FJsonObject> SpecStatus = MakeShared<FJsonObject>();
				SpecStatus->SetNumberField(TEXT("index"), NodeIdx);
				SpecStatus->SetStringField(TEXT("status"), TEXT("skipped_missing_id"));
				NodeSpecReport.Add(MakeShared<FJsonValueObject>(SpecStatus));
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
				TSharedPtr<FJsonObject> SpecStatus = MakeShared<FJsonObject>();
				SpecStatus->SetNumberField(TEXT("index"), NodeIdx);
				SpecStatus->SetStringField(TEXT("id"), LocalId);
				SpecStatus->SetStringField(TEXT("status"), TEXT("failed"));
				SpecStatus->SetStringField(TEXT("error"), R.Error);
				NodeSpecReport.Add(MakeShared<FJsonValueObject>(SpecStatus));

				// Carry the failing node's own warnings out with the error. They were
				// only collected on the success path below, so an unresolved
				// function_class -- the actual cause -- was dropped, leaving the
				// caller with the pin guard's generic "compile the Blueprint and
				// retry" advice and no mention of the name that failed to resolve.
				for (const FString& W : R.Warnings)
				{
					Warnings.Add(FString::Printf(TEXT("nodes[%d] '%s': %s"), NodeIdx, *LocalId, *W));
				}

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

			{
				TSharedPtr<FJsonObject> SpecStatus = MakeShared<FJsonObject>();
				SpecStatus->SetNumberField(TEXT("index"), NodeIdx);
				SpecStatus->SetStringField(TEXT("id"), LocalId);
				SpecStatus->SetStringField(TEXT("guid"), R.Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
				SpecStatus->SetStringField(TEXT("status"), TEXT("created"));
				NodeSpecReport.Add(MakeShared<FJsonValueObject>(SpecStatus));
			}

			UE_LOG(LogClaireon, Log, TEXT("[BP ApplyDelta] Created '%s' (%s) → GUID %s"),
				*LocalId,
				*R.Node->GetNodeTitle(ENodeTitleType::ListView).ToString(),
				*R.Node->NodeGuid.ToString());

			++NodeIdx;
		}
	}

	// ========================================================================
	// Phase 3.5: Pin defaults
	// ========================================================================
	// Runs after create (so local ids resolve) and before connect (so a pin that
	// is about to be wired can still be seeded, and so a bad default aborts before
	// any links are made). Failure rolls back the WHOLE batch, matching this tool's
	// all-or-nothing contract -- deliberately NOT apply_spec's per-entry model.
	{
		const TArray<TSharedPtr<FJsonValue>>* PinDefaults = nullptr;
		if (Ops->TryGetArrayField(TEXT("pin_defaults"), PinDefaults) && PinDefaults)
		{
			const UEdGraphSchema_K2* K2Schema = Cast<const UEdGraphSchema_K2>(Schema);
			int32 PinDefaultIdx = 0;
			for (const TSharedPtr<FJsonValue>& Entry : *PinDefaults)
			{
				TSharedPtr<FJsonObject> EntryObj = Entry.IsValid() ? Entry->AsObject() : nullptr;
				if (!EntryObj.IsValid())
				{
					return CancelAndError(FString::Printf(
						TEXT("pin_defaults[%d]: entry is not an object"), PinDefaultIdx));
				}

				FString NodeRef, PinName, Value;
				if (!EntryObj->TryGetStringField(TEXT("node"), NodeRef))
				{
					return CancelAndError(FString::Printf(
						TEXT("pin_defaults[%d]: missing required field 'node'"), PinDefaultIdx));
				}
				if (!EntryObj->TryGetStringField(TEXT("pin"), PinName))
				{
					return CancelAndError(FString::Printf(
						TEXT("pin_defaults[%d]: missing required field 'pin'"), PinDefaultIdx));
				}
				if (!EntryObj->TryGetStringField(TEXT("value"), Value))
				{
					return CancelAndError(FString::Printf(
						TEXT("pin_defaults[%d]: missing required field 'value'"), PinDefaultIdx));
				}

				FString FindErr;
				UEdGraphNode* Node = BPApplyDelta_ResolveNodeRef(NodeRef, LocalIdMap, Graph, FindErr);
				if (!IsValid(Node))
				{
					return CancelAndError(FString::Printf(
						TEXT("pin_defaults[%d] node '%s': %s"), PinDefaultIdx, *NodeRef, *FindErr));
				}

				UEdGraphPin* Pin = BPApplyDelta_FindPinOnNode(Node, PinName, FindErr);
				if (!Pin)
				{
					return CancelAndError(FString::Printf(
						TEXT("pin_defaults[%d] pin '%s': %s"), PinDefaultIdx, *PinName, *FindErr));
				}
				if (Pin->Direction != EGPD_Input)
				{
					return CancelAndError(FString::Printf(
						TEXT("pin_defaults[%d]: cannot set a default on output pin '%s'"), PinDefaultIdx, *PinName));
				}
				if (Pin->LinkedTo.Num() > 0)
				{
					return CancelAndError(FString::Printf(
						TEXT("pin_defaults[%d]: cannot set a default on connected pin '%s'"), PinDefaultIdx, *PinName));
				}

				Node->Modify();
				if (IsValid(K2Schema))
				{
					K2Schema->TrySetDefaultValue(*Pin, Value);
				}
				else
				{
					Schema->TrySetDefaultValue(*Pin, Value);
				}

				// TrySetDefaultValue swallows validation failures, so confirm the write
				// landed rather than reporting a success the graph does not reflect.
				if (Pin->DefaultValue != Value && Pin->DefaultObject == nullptr && Pin->DefaultTextValue.IsEmpty())
				{
					return CancelAndError(FString::Printf(
						TEXT("pin_defaults[%d]: '%s' was rejected as a default for pin '%s'"),
						PinDefaultIdx, *Value, *PinName));
				}

				Data->LastOperationAffectedNodes.Add(Node->NodeGuid);
				++PinDefaultIdx;
			}
		}
	}

	// ========================================================================
	// Phase 4: Connect
	// ========================================================================
	// accept the deprecated dict form {connections:[...]} in addition to the
	// correct array form. When a caller passes connections as an object that has a
	// nested "connections" array key, unwrap it and emit a deprecation warning so
	// the caller knows to switch to the array form.
	//
	// The canonical form is a top-level JSON array:
	//   "connections": [ {from, from_pin, to, to_pin}, ... ]
	// The deprecated dict form that arrived from older callers:
	//   "connections": { "connections": [ {from, from_pin, to, to_pin}, ... ] }
	TArray<TSharedPtr<FJsonValue>> ApplySpecDeltaStateTree_UnwrappedConnections; // storage for the unwrapped copy
	const TArray<TSharedPtr<FJsonValue>>* ConnDictFallback = nullptr;
	{
		const TSharedPtr<FJsonObject>* ConnDictPtr = nullptr;
		if (Ops->TryGetObjectField(TEXT("connections"), ConnDictPtr) && ConnDictPtr)
		{
			const TArray<TSharedPtr<FJsonValue>>* InnerArray = nullptr;
			if ((*ConnDictPtr)->TryGetArrayField(TEXT("connections"), InnerArray) && InnerArray)
			{
				Warnings.Add(TEXT("Deprecated: 'connections' was passed as a dict with a nested 'connections' array. Pass connections directly as a JSON array. The inner array was unwrapped for this call."));
				ApplySpecDeltaStateTree_UnwrappedConnections = *InnerArray;
				ConnDictFallback = &ApplySpecDeltaStateTree_UnwrappedConnections;
			}
			else
			{
				Warnings.Add(TEXT("'connections' was passed as an object but no inner 'connections' array was found; 0 connections will be made. Pass connections as a JSON array."));
			}
		}
	}
	int32 ConnectionsMade = 0;
	const TArray<TSharedPtr<FJsonValue>>* ConnArray = nullptr;
	if (!Ops->TryGetArrayField(TEXT("connections"), ConnArray))
	{
		ConnArray = ConnDictFallback;
	}
	if (ConnArray)
	{
		// Index-based so every abort path can record which connections[] entry
		// failed (indices count skipped entries too -- they match the caller's
		// array positions).
		for (int32 ConnIdx = 0; ConnIdx < ConnArray->Num(); ++ConnIdx)
		{
			const TSharedPtr<FJsonValue>& Entry = (*ConnArray)[ConnIdx];
			const TSharedPtr<FJsonObject>* ConnObj = nullptr;
			if (!Entry.IsValid() || !Entry->TryGetObject(ConnObj) || !ConnObj) continue;

			FString FromRef, FromPinName, ToRef, ToPinName;
			if (!(*ConnObj)->TryGetStringField(TEXT("from"), FromRef) ||
			    !(*ConnObj)->TryGetStringField(TEXT("from_pin"), FromPinName) ||
			    !(*ConnObj)->TryGetStringField(TEXT("to"), ToRef) ||
			    !(*ConnObj)->TryGetStringField(TEXT("to_pin"), ToPinName))
			{
				Warnings.Add(FString::Printf(TEXT("connections[%d] missing from/from_pin/to/to_pin — skipped"), ConnIdx));
				continue;
			}

			FString FindErr;
			UEdGraphNode* FromNode = BPApplyDelta_ResolveNodeRef(FromRef, LocalIdMap, Graph, FindErr);
			if (!IsValid(FromNode))
			{
				FailingConnectionIndex = ConnIdx;
				return CancelAndError(FString::Printf(TEXT("connections[%d] from '%s': %s"), ConnIdx, *FromRef, *FindErr));
			}
			UEdGraphNode* ToNode = BPApplyDelta_ResolveNodeRef(ToRef, LocalIdMap, Graph, FindErr);
			if (!IsValid(ToNode))
			{
				FailingConnectionIndex = ConnIdx;
				return CancelAndError(FString::Printf(TEXT("connections[%d] to '%s': %s"), ConnIdx, *ToRef, *FindErr));
			}

			UEdGraphPin* FromPin = BPApplyDelta_FindPinOnNode(FromNode, FromPinName, FindErr);
			if (!FromPin)
			{
				FailingConnectionIndex = ConnIdx;
				return CancelAndError(FString::Printf(TEXT("connections[%d] %s.%s: %s"), ConnIdx, *FromRef, *FromPinName, *FindErr));
			}
			UEdGraphPin* ToPin = BPApplyDelta_FindPinOnNode(ToNode, ToPinName, FindErr);
			if (!ToPin)
			{
				FailingConnectionIndex = ConnIdx;
				return CancelAndError(FString::Printf(TEXT("connections[%d] %s.%s: %s"), ConnIdx, *ToRef, *ToPinName, *FindErr));
			}

			const FPinConnectionResponse Resp = Schema->CanCreateConnection(FromPin, ToPin);
			if (Resp.Response == CONNECT_RESPONSE_DISALLOW)
			{
				FailingConnectionIndex = ConnIdx;
				return CancelAndError(FString::Printf(TEXT("connections[%d]: cannot connect %s.%s → %s.%s: %s"),
					ConnIdx, *FromRef, *FromPinName, *ToRef, *ToPinName, *Resp.Message.ToString()));
			}

			if (Resp.Response == CONNECT_RESPONSE_BREAK_OTHERS_A) FromPin->BreakAllPinLinks();
			if (Resp.Response == CONNECT_RESPONSE_BREAK_OTHERS_B) ToPin->BreakAllPinLinks();

			if (!Schema->TryCreateConnection(FromPin, ToPin))
			{
				FailingConnectionIndex = ConnIdx;
				return CancelAndError(FString::Printf(TEXT("connections[%d]: TryCreateConnection failed: %s.%s → %s.%s"),
					ConnIdx, *FromRef, *FromPinName, *ToRef, *ToPinName));
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

	// Build response by reusing the bp session response builder -- we go through
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
