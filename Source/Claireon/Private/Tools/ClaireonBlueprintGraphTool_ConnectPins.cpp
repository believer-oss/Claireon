// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT


#include "Tools/ClaireonBlueprintGraphTool_ConnectPins.h"
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
#include "K2Node.h"
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


FString ClaireonBlueprintGraphTool_ConnectPins::GetOperation() const { return TEXT("connect_pins"); }

TArray<FString> ClaireonBlueprintGraphTool_ConnectPins::GetSearchKeywords() const
{
    return {TEXT("bp"), TEXT("pin"), TEXT("connect"), TEXT("wire"), TEXT("link"), TEXT("graph"), TEXT("node")};
}

FString ClaireonBlueprintGraphTool_ConnectPins::GetDescription() const
{
    return TEXT("Connect two pins on the current session's graph. Accepts node GUIDs or titles plus pin names; pin names are fuzzy-resolved (e.g. 'exec' matches 'execute', 'then' matches the canonical exec output). Most-common pitfall: forgetting that auto_connect_from_cursor on bp_add_node typically obviates this call. Accepts either session_id or asset_path; auto-opens a session when asset_path is supplied.");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ConnectPins::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddString(TEXT("session_id"), TEXT("Session id from a prior open/create (or use asset_path to auto-open)."), false);
    Builder.AddString(TEXT("asset_path"), TEXT("Blueprint asset path (alternative to session_id)."), false);
    Builder.AddString(TEXT("source_node_title"), TEXT("Title of the source node."));
    Builder.AddString(TEXT("source_node_guid"), TEXT("GUID of the source node (alternative to source_node_title)."));
    Builder.AddString(TEXT("source_pin_name"), TEXT("Source pin name."), true);
    Builder.AddString(TEXT("target_node_title"), TEXT("Title of the target node."));
    Builder.AddString(TEXT("target_node_guid"), TEXT("GUID of the target node (alternative to target_node_title)."));
    Builder.AddString(TEXT("target_pin_name"), TEXT("Target pin name."), true);
    // from_*/to_* aliases. bp_apply_delta connections and bp_get_graph output both
    // speak from/to, so callers reach for that spelling here too. Purely additive:
    // the canonical source_*/target_* names win when both are supplied.
    Builder.AddString(TEXT("from_node"), TEXT("Alias for source_node_guid / source_node_title (accepts either a GUID or a title). Ignored when a source_node_* field is present."));
    Builder.AddString(TEXT("from_pin"), TEXT("Alias for source_pin_name. Ignored when source_pin_name is present."));
    Builder.AddString(TEXT("to_node"), TEXT("Alias for target_node_guid / target_node_title (accepts either a GUID or a title). Ignored when a target_node_* field is present."));
    Builder.AddString(TEXT("to_pin"), TEXT("Alias for target_pin_name. Ignored when target_pin_name is present."));
    // Read at Execute and never declared until now: an input/output pair sharing
    // a pin name needed the hint, and supplying it was a silent no-op.
    Builder.AddString(TEXT("source_pin_direction"), TEXT("Disambiguates the source pin when input and output share a name: 'input' | 'output'."));
    Builder.AddString(TEXT("target_pin_direction"), TEXT("Disambiguates the target pin when input and output share a name: 'input' | 'output'."));
    Builder.AddString(TEXT("response_mode"), TEXT("Response verbosity: 'full' | 'changed' | 'status' (default 'changed')."));
    return Builder.Build();
}

FToolResult ClaireonBlueprintGraphTool_ConnectPins::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FBlueprintEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("connect_pins"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return CheckMutationAffectedNodes(TEXT("connect_pins"), Data, ConnectPins_Impl(SessionId, Data, Params));
}

FToolResult ClaireonBlueprintGraphTool_ConnectPins::ConnectPins_Impl(
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

	// Normalize the from_*/to_* alias spelling into the canonical source_*/target_*
	// fields before anything reads them, so the rest of this function stays
	// single-spelling. Canonical wins on conflict: the aliases are a compatibility
	// layer, and silently preferring the real name avoids inventing a new error for
	// callers who pass both.
	{
		// One alias key has to accept both a GUID and a title, and the two canonical
		// fields take different lookup paths -- the title path does NOT fall back to a
		// GUID parse. Route by shape: hyphen-stripped all-hex and long enough to be a
		// GUID prefix means GUID, anything else is a title.
		auto LooksLikeNodeGuid = [](const FString& Value) -> bool
		{
			const FString Hex = Value.Replace(TEXT("-"), TEXT(""));
			if (Hex.Len() < 8 || Hex.Len() > 32) { return false; }
			for (int32 I = 0; I < Hex.Len(); ++I)
			{
				if (!FChar::IsHexDigit(Hex[I])) { return false; }
			}
			return true;
		};

		auto ApplyNodeAlias = [&Params, &LooksLikeNodeGuid](
			const TCHAR* AliasKey, const TCHAR* GuidKey, const TCHAR* TitleKey)
		{
			if (Params->HasField(GuidKey) || Params->HasField(TitleKey))
			{
				return;
			}
			FString AliasValue;
			if (!Params->TryGetStringField(AliasKey, AliasValue) || AliasValue.IsEmpty())
			{
				return;
			}
			Params->SetStringField(LooksLikeNodeGuid(AliasValue) ? GuidKey : TitleKey, AliasValue);
		};

		auto ApplyPinAlias = [&Params](const TCHAR* AliasKey, const TCHAR* CanonicalKey)
		{
			if (Params->HasField(CanonicalKey))
			{
				return;
			}
			FString AliasValue;
			if (Params->TryGetStringField(AliasKey, AliasValue))
			{
				Params->SetStringField(CanonicalKey, AliasValue);
			}
		};

		ApplyNodeAlias(TEXT("from_node"), TEXT("source_node_guid"), TEXT("source_node_title"));
		ApplyPinAlias(TEXT("from_pin"), TEXT("source_pin_name"));
		ApplyNodeAlias(TEXT("to_node"), TEXT("target_node_guid"), TEXT("target_node_title"));
		ApplyPinAlias(TEXT("to_pin"), TEXT("target_pin_name"));
	}

	// Get source pin name (required)
	FString SourcePinName;
	if (!Params->TryGetStringField(TEXT("source_pin_name"), SourcePinName))
	{
		return MakeErrorResult(TEXT("Missing required field: source_pin_name (or its alias from_pin)"));
	}

	// Get target pin name (required)
	FString TargetPinName;
	if (!Params->TryGetStringField(TEXT("target_pin_name"), TargetPinName))
	{
		return MakeErrorResult(TEXT("Missing required field: target_pin_name (or its alias to_pin)"));
	}

	// Find source node (by GUID or title)
	UEdGraphNode* SourceNode = nullptr;
	FString SourceNodeGuidStr, SourceNodeTitle;

	if (Params->TryGetStringField(TEXT("source_node_guid"), SourceNodeGuidStr))
	{
		// Find by GUID (full GUID or >=8-hex prefix)
		FString ResolveError;
		SourceNode = ClaireonBPGraphInternal::FindNodeForOperationStr(Graph, SourceNodeGuidStr, Data, ResolveError, TEXT("source_node_guid"));
		if (!IsValid(SourceNode))
		{
			return MakeErrorResult(ResolveError);
		}
	}
	else if (Params->TryGetStringField(TEXT("source_node_title"), SourceNodeTitle))
	{
		// Find by title
		TArray<UEdGraphNode*> MatchingNodes = ClaireonBlueprintHelpers::FindNodesByTitle(Graph, SourceNodeTitle, true);
		if (MatchingNodes.Num() != 1)
		{
			return MakeErrorResult(ClaireonBlueprintHelpers::FormatTitleMatchFailure(
				Graph, SourceNodeTitle, MatchingNodes, TEXT("source_node_guid")));
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
		// Find by GUID (full GUID or >=8-hex prefix)
		FString ResolveError;
		TargetNode = ClaireonBPGraphInternal::FindNodeForOperationStr(Graph, TargetNodeGuidStr, Data, ResolveError, TEXT("target_node_guid"));
		if (!IsValid(TargetNode))
		{
			return MakeErrorResult(ResolveError);
		}
	}
	else if (Params->TryGetStringField(TEXT("target_node_title"), TargetNodeTitle))
	{
		// Find by title
		TArray<UEdGraphNode*> MatchingNodes = ClaireonBlueprintHelpers::FindNodesByTitle(Graph, TargetNodeTitle, true);
		if (MatchingNodes.Num() != 1)
		{
			return MakeErrorResult(ClaireonBlueprintHelpers::FormatTitleMatchFailure(
				Graph, TargetNodeTitle, MatchingNodes, TEXT("target_node_guid")));
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

	// Use the schema's canonical TryCreateConnection path rather than the
	// raw MakeLinkTo + manual notification. TryCreateConnection:
	//   1. Calls CanCreateConnection and honours BREAK_OTHERS_A/B/AB responses.
	//   2. Calls both MakeLinkTo endpoints.
	//   3. Calls PinConnectionListChanged on both endpoints (the per-pin hook).
	//   4. On K2 graphs, NotifyPinConnectionListChanged is invoked via the
	//      K2 schema override to propagate wildcard types through
	//      UK2Node_CallArrayFunction, UK2Node_Select, UK2Node_MakeArray, etc.
	// This is the path the Blueprint editor itself uses, so going through it
	// keeps Claireon's behaviour identical to the UI's.
	const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();

	// Pre-check so we can surface a clean error without a transaction and
	// without mutating the graph.
	const FPinConnectionResponse PreCheck = K2Schema->CanCreateConnection(SourcePin, TargetPin);
	if (PreCheck.Response == CONNECT_RESPONSE_DISALLOW)
	{
		return MakeErrorResult(FString::Printf(TEXT("Cannot connect pins: %s"), *PreCheck.Message.ToString()));
	}

	// Make the connection using transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Connect Blueprint Pins")));
	Blueprint->Modify();
	Graph->Modify();

	const bool bConnectionMade = K2Schema->TryCreateConnection(SourcePin, TargetPin);
	if (!bConnectionMade)
	{
		return MakeErrorResult(TEXT("TryCreateConnection failed (schema rejected the link)"));
	}

	// Explicit wildcard resolution sweep. TryCreateConnection's K2 schema path
	// resolves wildcards on UK2Node_CallArrayFunction / UK2Node_Select / UK2Node_MakeArray,
	// but generic wildcard pins on tunnels/knots/macro instances can survive with category
	// "wildcard" after the link is made. Run a fixed-point loop: for each wildcard pin still
	// holding category=wildcard, if its linked neighbors have a resolved category, propagate
	// the neighbor's type onto the pin. NotifyPinConnectionListChanged on both nodes lets the
	// owning K2 node re-coerce sibling pins. 16-iter cap guards against pathological cycles
	// in macro graphs.
	ClaireonBlueprintHelpers::PropagateWildcardTypesViaLinks({SourceNode, TargetNode});

	// TryCreateConnection only fires the per-pin PinConnectionListChanged hooks;
	// nodes that defer their rebuild to the NODE-level hook (UK2Node_Select sets
	// bReconstructNode in OnPinTypeChanged and consumes it in
	// NodeConnectionListChanged -- that is where enum-index Selects grow their
	// per-entry option pins) never rebuild without this. The graph editor UI
	// calls it after every drag-connect; mirror that.
	SourceNode->NodeConnectionListChanged();
	TargetNode->NodeConnectionListChanged();

	// Capture node titles AFTER TryCreateConnection has run (wildcard
	// propagation can rebuild pin arrays, but the node objects survive).
	const FString SourceTitle = SourceNode->GetNodeTitle(ENodeTitleType::ListView).ToString();
	const FString TargetTitle = TargetNode->GetNodeTitle(ENodeTitleType::ListView).ToString();

	Data->Cursor.LastOperationStatus = FString::Printf(
		TEXT("Connected: [%s].%s -> [%s].%s"),
		*SourceTitle, *SourcePinName,
		*TargetTitle, *TargetPinName);

	// Mark Blueprint as modified
	FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);

	// Populate affected nodes: both endpoint nodes
	Data->LastOperationAffectedNodes.Add(SourceNode->NodeGuid);
	Data->LastOperationAffectedNodes.Add(TargetNode->NodeGuid);

	// Wildcard propagation may re-type sibling pins and trigger ReconstructNode
	// on the endpoint nodes, which can re-link to (or unlink from) neighbors.
	// Add any currently-linked neighbor GUIDs to the affected set so the diff
	// response reflects the full extent of the change.
	auto AddLinkedNeighborGuids = [Data](UEdGraphNode* EndpointNode)
	{
		if (!IsValid(EndpointNode)) { return; }
		for (UEdGraphPin* Pin : EndpointNode->Pins)
		{
			if (!Pin) { continue; }
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked)
				{
					if (UEdGraphNode* Neighbor = Linked->GetOwningNodeUnchecked(); IsValid(Neighbor))
					{
						Data->LastOperationAffectedNodes.Add(Neighbor->NodeGuid);
					}
				}
			}
		}
	};
	AddLinkedNeighborGuids(SourceNode);
	AddLinkedNeighborGuids(TargetNode);

	FToolResult ConnectResult = BuildStateResponse(SessionId, Data);
	ConnectResult.Warnings.Append(ResolutionWarnings);
	return ConnectResult;
}

// ----------------------------------------------------------------------------
// hot-path metadata enrichment
// ----------------------------------------------------------------------------

FString ClaireonBlueprintGraphTool_ConnectPins::GetFullDescription() const
{
    return TEXT(
        "Connects two pins on the current session's graph. Accepts node GUIDs "
        "(stable across saves) or human-readable node titles plus pin names. "
        "Pin names are fuzzy-resolved: 'exec' matches the canonical exec input, "
        "'then' matches the canonical exec output, and partial substring "
        "matches resolve as long as they are unambiguous. Per the per-node "
        "cycle in .claude/areas/blueprint-editing.md, prefer auto_connect_from_cursor=true "
        "on bp_add_node to wire as you go; an explicit "
        "connect_pins call is mainly needed when joining two pre-existing "
        "nodes or wiring data pins that the cursor cannot route automatically.");
}

FString ClaireonBlueprintGraphTool_ConnectPins::GetExampleUsage() const
{
    return TEXT(
        "bp_connect_pins session_id=\"...\" "
        "source_node_title=\"PrintString_0\" source_pin_name=\"then\" "
        "target_node_title=\"DelayUntilNextTick_1\" target_pin_name=\"exec\"");
}

TSharedPtr<FJsonObject> ClaireonBlueprintGraphTool_ConnectPins::GetParameterTooltips() const
{
    TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
    T->SetStringField(TEXT("session_id"), TEXT("Session ID returned by bp_open or _create."));
    T->SetStringField(TEXT("source_node_title"), TEXT("Source node title (also accepted as 'from_node', which takes a GUID or a title)."));
    T->SetStringField(TEXT("source_node_guid"), TEXT("Source node GUID, full or >=8-hex prefix (alternative to source_node_title)."));
    T->SetStringField(TEXT("source_pin_name"), TEXT("Source pin name (also accepted as 'from_pin'). Fuzzy-resolved ('exec', 'then', partial substrings)."));
    T->SetStringField(TEXT("target_node_title"), TEXT("Target node title (also accepted as 'to_node', which takes a GUID or a title)."));
    T->SetStringField(TEXT("target_node_guid"), TEXT("Target node GUID, full or >=8-hex prefix (alternative to target_node_title)."));
    T->SetStringField(TEXT("target_pin_name"), TEXT("Target pin name (also accepted as 'to_pin'). Fuzzy-resolved."));
    T->SetStringField(TEXT("from_node"), TEXT("Alias for source_node_guid / source_node_title. Ignored when either canonical field is present."));
    T->SetStringField(TEXT("from_pin"), TEXT("Alias for source_pin_name. Ignored when source_pin_name is present."));
    T->SetStringField(TEXT("to_node"), TEXT("Alias for target_node_guid / target_node_title. Ignored when either canonical field is present."));
    T->SetStringField(TEXT("to_pin"), TEXT("Alias for target_pin_name. Ignored when target_pin_name is present."));
    return T;
}

#undef LOCTEXT_NAMESPACE
