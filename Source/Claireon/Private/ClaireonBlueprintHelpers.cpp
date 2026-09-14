// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonBlueprintHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonNameResolver.h"
#include "Dom/JsonObject.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/InheritableComponentHandler.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Components/ActorComponent.h"
#include "Engine/EngineTypes.h"        // ELifetimeCondition, for the replication_condition field
#include "Engine/MemberReference.h"
#include "UObject/ReflectedTypeAccessors.h" // StaticEnum<>, same
#include "Blueprint/BlueprintExtension.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_AsyncAction.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CallArrayFunction.h"
#include "K2Node_CallDataTableFunction.h"
#include "K2Node_CallMaterialParameterCollectionFunction.h"
#include "K2Node_CommutativeAssociativeBinaryOperator.h"
#include "K2Node_Event.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "K2Node_MakeContainer.h"
#include "GameplayTask.h"
#include "Abilities/Tasks/AbilityTask.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "BlueprintEditor.h"
#include "GraphEditor.h"
#include "Widgets/Docking/SDockTab.h"
#include "GameplayTagContainer.h"
#include "StructUtils/InstancedStruct.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"


FScopedBlueprintEditor::FScopedBlueprintEditor(UBlueprint* InBlueprint, bool /*bInSilent*/, bool bInCloseOnDestroy)
	: FClaireonScopedAssetEditor(InBlueprint, bInCloseOnDestroy)
{
}

TSharedPtr<FBlueprintEditor> FScopedBlueprintEditor::GetBlueprintEditor() const
{
	TSharedPtr<FAssetEditorToolkit> OpenToolkit = GetToolkit();
	return OpenToolkit.IsValid() ? StaticCastSharedPtr<FBlueprintEditor>(OpenToolkit) : nullptr;
}

TSharedPtr<SGraphEditor> FScopedBlueprintEditor::GetGraphEditor(UEdGraph* Graph)
{
	TSharedPtr<FBlueprintEditor> BlueprintEditor = GetBlueprintEditor();
	if (!BlueprintEditor.IsValid() || !::IsValid(Graph))
	{
		return nullptr;
	}

	return BlueprintEditor->OpenGraphAndBringToFront(Graph, /*bSetFocus=*/false);
}


const TCHAR* ClaireonEditorBindingStatusToWireString(EClaireonEditorBindingStatus Status)
{
	switch (Status)
	{
	case EClaireonEditorBindingStatus::Valid:             return TEXT("valid");
	case EClaireonEditorBindingStatus::NotBound:          return TEXT("not_bound");
	case EClaireonEditorBindingStatus::BoundEditorClosed: return TEXT("bound_editor_closed");
	case EClaireonEditorBindingStatus::GraphTabClosed:    return TEXT("graph_tab_closed");
	case EClaireonEditorBindingStatus::GraphRemoved:      return TEXT("graph_removed");
	}
	return TEXT("unknown");
}

#if WITH_CLAIREON_TESTS
namespace ClaireonBlueprintEditorBindingSeam
{
	struct FSeamCandidate
	{
		TWeakObjectPtr<UEdGraph>  Graph;
		TSharedPtr<SGraphEditor>  Widget;
	};

	// Prefix seam helpers to avoid collisions in unity builds.
	inline TMap<FGuid, FSeamCandidate>& BindingSeam_Candidates()
	{
		static TMap<FGuid, FSeamCandidate> Candidates;
		return Candidates;
	}

	FGuid RegisterInstance(UEdGraph* Graph, TSharedPtr<SGraphEditor> Widget)
	{
		const FGuid InstanceId = FGuid::NewGuid();
		BindingSeam_Candidates().Add(InstanceId, FSeamCandidate{ Graph, Widget });
		return InstanceId;
	}

	void CloseInstance(const FGuid& InstanceId)
	{
		BindingSeam_Candidates().Remove(InstanceId);
	}

	void CloseGraphTab(const FGuid& InstanceId)
	{
		if (FSeamCandidate* Candidate = BindingSeam_Candidates().Find(InstanceId))
		{
			Candidate->Widget.Reset();
		}
	}

	TSharedPtr<SGraphEditor> FindWidget(const FGuid& InstanceId)
	{
		FSeamCandidate* Candidate = BindingSeam_Candidates().Find(InstanceId);
		return Candidate ? Candidate->Widget : nullptr;
	}

	bool IsInstanceRegistered(const FGuid& InstanceId)
	{
		return BindingSeam_Candidates().Contains(InstanceId);
	}

	void Reset()
	{
		BindingSeam_Candidates().Empty();
	}

	FScopedSeam::~FScopedSeam()
	{
		Reset();
	}
}
#endif // WITH_CLAIREON_TESTS

void FClaireonBlueprintEditorBinding::Clear()
{
	Blueprint.Reset();
	BoundGraph.Reset();
	Editor.Reset();
	GraphEditor.Reset();
	InstanceId.Invalidate();
	bSeamBound = false;
}

void FClaireonBlueprintEditorBinding::BindTo(UBlueprint* InBlueprint, UEdGraph* InGraph,
	TSharedPtr<FBlueprintEditor> InEditor, TSharedPtr<SGraphEditor> InGraphEditor)
{
	Clear();
	if (!InEditor.IsValid())
	{
		return;
	}

	Blueprint = InBlueprint;
	BoundGraph = InGraph;
	Editor = InEditor;
	GraphEditor = InGraphEditor;
	// Use a fresh ID so a reused toolkit address cannot revive a stale binding.
	InstanceId = FGuid::NewGuid();
}

#if WITH_CLAIREON_TESTS
void FClaireonBlueprintEditorBinding::BindToSeamInstance(UBlueprint* InBlueprint, UEdGraph* InGraph, const FGuid& InInstanceId)
{
	Clear();
	Blueprint = InBlueprint;
	BoundGraph = InGraph;
	InstanceId = InInstanceId;
	bSeamBound = true;
}
#endif

EClaireonEditorBindingStatus FClaireonBlueprintEditorBinding::Revalidate(UEdGraph* SessionGraph) const
{
	if (!InstanceId.IsValid())
	{
		return EClaireonEditorBindingStatus::NotBound;
	}

	// Check instance and graph lifetime before tab state.
#if WITH_CLAIREON_TESTS
	if (bSeamBound)
	{
		if (!ClaireonBlueprintEditorBindingSeam::IsInstanceRegistered(InstanceId))
		{
			return EClaireonEditorBindingStatus::BoundEditorClosed;
		}
		if (!::IsValid(SessionGraph))
		{
			return EClaireonEditorBindingStatus::GraphRemoved;
		}
		if (!ClaireonBlueprintEditorBindingSeam::FindWidget(InstanceId).IsValid())
		{
			return EClaireonEditorBindingStatus::GraphTabClosed;
		}
		return EClaireonEditorBindingStatus::Valid;
	}
#endif

	TSharedPtr<FBlueprintEditor> PinnedEditor = Editor.Pin();
	UBlueprint* OwningBlueprint = Blueprint.Get();
	if (!PinnedEditor.IsValid() || !::IsValid(OwningBlueprint)
		|| !ClaireonAssetEditorWindow::IsInstanceRegistered(OwningBlueprint, PinnedEditor.Get()))
	{
		return EClaireonEditorBindingStatus::BoundEditorClosed;
	}

	if (!::IsValid(SessionGraph))
	{
		return EClaireonEditorBindingStatus::GraphRemoved;
	}

	// Match graph identity, including nested composites.
	TArray<UEdGraph*> AllGraphs;
	OwningBlueprint->GetAllGraphs(AllGraphs);
	if (!AllGraphs.Contains(SessionGraph))
	{
		return EClaireonEditorBindingStatus::GraphRemoved;
	}

	// Only the originally bound graph requires an existing tab; retargeted graphs open during resolution.
	if (SessionGraph != BoundGraph.Get())
	{
		return EClaireonEditorBindingStatus::Valid;
	}

	// Inspect tabs without opening one, so a closed tab remains detectable.
	TArray<TSharedPtr<SDockTab>> OpenTabs;
	if (!PinnedEditor->FindOpenTabsContainingDocument(SessionGraph, OpenTabs) || OpenTabs.Num() == 0)
	{
		return EClaireonEditorBindingStatus::GraphTabClosed;
	}

	return EClaireonEditorBindingStatus::Valid;
}

TSharedPtr<SGraphEditor> FClaireonBlueprintEditorBinding::ResolveGraphEditor(UEdGraph* SessionGraph, EClaireonEditorBindingStatus& OutStatus)
{
	OutStatus = Revalidate(SessionGraph);
	if (OutStatus != EClaireonEditorBindingStatus::Valid)
	{
		return nullptr;
	}

#if WITH_CLAIREON_TESTS
	if (bSeamBound)
	{
		return ClaireonBlueprintEditorBindingSeam::FindWidget(InstanceId);
	}
#endif

	TSharedPtr<SGraphEditor> Cached = GraphEditor.Pin();
	if (Cached.IsValid() && Cached->GetCurrentGraph() == SessionGraph)
	{
		return Cached;
	}

	// Resolve through the bound editor when the graph changes or its widget is rebuilt.
	TSharedPtr<FBlueprintEditor> PinnedEditor = Editor.Pin();
	if (!PinnedEditor.IsValid())
	{
		OutStatus = EClaireonEditorBindingStatus::BoundEditorClosed;
		return nullptr;
	}

	TSharedPtr<SGraphEditor> Resolved = PinnedEditor->OpenGraphAndBringToFront(SessionGraph, /*bSetFocus=*/false);
	if (!Resolved.IsValid())
	{
		OutStatus = EClaireonEditorBindingStatus::GraphTabClosed;
		return nullptr;
	}

	BoundGraph = SessionGraph;
	GraphEditor = Resolved;
	return Resolved;
}


namespace ClaireonBlueprintHelpers
{
	TArray<UEdGraphPin*> GetExecPins(UEdGraphNode* Node, bool bInputOnly, bool bOutputOnly)
	{
		TArray<UEdGraphPin*> ExecPins;

		if (!IsValid(Node))
		{
			return ExecPins;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				if (bInputOnly && Pin->Direction != EGPD_Input)
				{
					continue;
				}
				if (bOutputOnly && Pin->Direction != EGPD_Output)
				{
					continue;
				}
				ExecPins.Add(Pin);
			}
		}

		return ExecPins;
	}

	bool HasExecInputPins(UEdGraphNode* Node)
	{
		if (!IsValid(Node))
		{
			return false;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Input)
			{
				return true;
			}
		}

		return false;
	}

	bool HasExecOutputPins(UEdGraphNode* Node)
	{
		if (!IsValid(Node))
		{
			return false;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec && Pin->Direction == EGPD_Output)
			{
				return true;
			}
		}

		return false;
	}

	TArray<UEdGraphNode*> FindRootNodes(UEdGraph* Graph)
	{
		TArray<UEdGraphNode*> RootNodes;

		if (!IsValid(Graph))
		{
			return RootNodes;
		}

		// Root nodes are nodes that have exec output pins but no exec input pins
		// These are typically Event nodes or entry points
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsValid(Node) && HasExecOutputPins(Node) && !HasExecInputPins(Node))
			{
				RootNodes.Add(Node);
			}
		}

		return RootNodes;
	}

	bool ValidateAssetPath(const FString& AssetPath, FString& OutError)
	{
		auto Result = ClaireonPathResolver::Resolve(AssetPath);
		if (!Result.bSuccess)
		{
			OutError = Result.Error;
			return false;
		}
		return true;
	}

	UEdGraphNode* FindNodeByGuid(const UEdGraph* Graph, const FGuid& NodeGuid, FGuid* OutCorrectedGuid)
	{
		if (!IsValid(Graph) || !NodeGuid.IsValid())
		{
			return nullptr;
		}

		// Exact match
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsValid(Node) && Node->NodeGuid == NodeGuid)
			{
				return Node;
			}
		}

		// Fallback: match on the A field only. Blueprint recompilation can
		// regenerate B/C/D while preserving A, causing GUIDs returned by
		// bp_get_graph to go stale by the time a graph-editing tool
		// tries to resolve them.  If exactly one node shares the A field we
		// treat it as the same logical node and log a warning.
		UEdGraphNode* AFieldMatch = nullptr;
		int32 AFieldMatchCount = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsValid(Node) && Node->NodeGuid.A == NodeGuid.A)
			{
				AFieldMatch = Node;
				++AFieldMatchCount;
			}
		}

		if (AFieldMatchCount == 1 && IsValid(AFieldMatch))
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[FindNodeByGuid] Exact GUID match failed — recovered via A-field fallback. ")
				TEXT("Requested=%s  Matched=%s  Node='%s'. ")
				TEXT("This usually means the blueprint was recompiled between get and edit calls."),
				*NodeGuid.ToString(),
				*AFieldMatch->NodeGuid.ToString(),
				*AFieldMatch->GetNodeTitle(ENodeTitleType::ListView).ToString());
			if (OutCorrectedGuid)
			{
				*OutCorrectedGuid = AFieldMatch->NodeGuid;
			}
			return AFieldMatch;
		}

		return nullptr;
	}

	bool ResolveNodeGuidString(const UEdGraph* Graph, const FString& GuidStr, UEdGraphNode*& OutNode,
	                           FString& OutError, const TCHAR* FieldNameForErrors, FGuid* OutCorrectedFullGuid)
	{
		OutNode = nullptr;
		if (OutCorrectedFullGuid)
		{
			*OutCorrectedFullGuid = FGuid();
		}

		if (!IsValid(Graph))
		{
			OutError = TEXT("ResolveNodeGuidString: Graph is null");
			return false;
		}

		const TCHAR* FieldName = FieldNameForErrors ? FieldNameForErrors : TEXT("node_guid");

		// 1. Full-GUID parse first. Preserves the A-field recompile-recovery path
		//    (and its correction out-param) that FindNodeByGuid provides.
		FGuid ParsedGuid;
		if (FGuid::Parse(GuidStr, ParsedGuid) && ParsedGuid.IsValid())
		{
			FGuid CorrectedGuid;
			if (UEdGraphNode* Found = FindNodeByGuid(Graph, ParsedGuid, &CorrectedGuid); IsValid(Found))
			{
				OutNode = Found;
				if (CorrectedGuid.IsValid() && OutCorrectedFullGuid)
				{
					*OutCorrectedFullGuid = CorrectedGuid;
				}
				return true;
			}
			OutError = FString::Printf(
				TEXT("Node %s not found in graph '%s'. Available nodes: %s"),
				*GuidStr, *Graph->GetName(), *FormatAvailableNodes(const_cast<UEdGraph*>(Graph)));
			return false;
		}

		// 2. Full parse failed: a valid hex prefix is 8..32 hex chars once hyphens
		//    are stripped. Anything shorter or non-hex is genuinely malformed input.
		FString HexPrefix = GuidStr.Replace(TEXT("-"), TEXT(""));
		bool bValidHexPrefix = HexPrefix.Len() >= 8 && HexPrefix.Len() <= 32;
		if (bValidHexPrefix)
		{
			for (int32 I = 0; I < HexPrefix.Len(); ++I)
			{
				if (!FChar::IsHexDigit(HexPrefix[I]))
				{
					bValidHexPrefix = false;
					break;
				}
			}
		}
		if (!bValidHexPrefix)
		{
			OutError = FString::Printf(TEXT("Invalid %s format: %s"), FieldName, *GuidStr);
			return false;
		}
		HexPrefix = HexPrefix.ToUpper();

		// 3. Prefix-match against every node's hyphen-free Digits GUID.
		TArray<UEdGraphNode*> Matches;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsValid(Node) && Node->NodeGuid.ToString(EGuidFormats::Digits).StartsWith(HexPrefix, ESearchCase::IgnoreCase))
			{
				Matches.Add(Node);
			}
		}

		if (Matches.Num() == 1)
		{
			OutNode = Matches[0];
			return true;
		}
		if (Matches.Num() == 0)
		{
			OutError = FString::Printf(
				TEXT("No node with GUID prefix '%s' found in graph '%s'. Available nodes: %s"),
				*GuidStr, *Graph->GetName(), *FormatAvailableNodes(const_cast<UEdGraph*>(Graph)));
			return false;
		}

		// Shares FormatAmbiguousNodeMatch with the title path: same answer shape, and the
		// candidate list now carries titles alongside GUIDs rather than bare GUIDs, which
		// is what lets the caller tell the candidates apart.
		OutError = FormatAmbiguousNodeMatch(
			const_cast<UEdGraph*>(Graph), TEXT("GUID prefix"), GuidStr, Matches,
			TEXT("Provide more characters or the full GUID."));
		return false;
	}

	void PropagateWildcardTypesViaLinks(TArray<UEdGraphNode*> SeedNodes, int32 MaxIterations)
	{
		// Extracted verbatim from ClaireonBlueprintGraphTool_ConnectPins so the literal-write
		// path in SetPinValue can run the same forward propagation after promoting a pin.
		//
		// TryCreateConnection's K2 schema path resolves wildcards on UK2Node_CallArrayFunction /
		// UK2Node_Select / UK2Node_MakeArray, but generic wildcard pins on tunnels/knots/macro
		// instances can survive with category "wildcard" after the link is made. Fixed-point
		// loop: for each wildcard pin still holding category=wildcard, if its linked neighbors
		// have a resolved category, propagate the neighbor's type onto the pin.
		// NotifyPinConnectionListChanged on the owner lets the K2 node re-coerce sibling pins.
		// The iteration cap guards against pathological cycles in macro graphs.
		const FName WildcardCat = UEdGraphSchema_K2::PC_Wildcard;
		auto IsWildcard = [&WildcardCat](const UEdGraphPin* Pin) -> bool
		{
			return Pin && Pin->PinType.PinCategory == WildcardCat;
		};

		for (int32 Iter = 0; Iter < MaxIterations; ++Iter)
		{
			bool bChangedAny = false;

			TArray<UEdGraphNode*> NodesToScan;
			for (UEdGraphNode* Seed : SeedNodes)
			{
				if (IsValid(Seed)) { NodesToScan.AddUnique(Seed); }
			}
			// Expand to include any node touched by our seeds' current links.
			for (UEdGraphNode* Seed : SeedNodes)
			{
				if (!IsValid(Seed)) { continue; }
				for (UEdGraphPin* Pin : Seed->Pins)
				{
					if (!Pin) { continue; }
					for (UEdGraphPin* Linked : Pin->LinkedTo)
					{
						if (Linked) { NodesToScan.AddUnique(Linked->GetOwningNodeUnchecked()); }
					}
				}
			}

			for (UEdGraphNode* N : NodesToScan)
			{
				if (!IsValid(N)) { continue; }
				for (UEdGraphPin* WildPin : N->Pins)
				{
					if (!IsWildcard(WildPin) || WildPin->LinkedTo.Num() == 0) { continue; }
					// Find a linked neighbor whose category is resolved.
					const UEdGraphPin* ResolvedNeighbor = nullptr;
					for (UEdGraphPin* Linked : WildPin->LinkedTo)
					{
						if (Linked && Linked->PinType.PinCategory != WildcardCat)
						{
							ResolvedNeighbor = Linked;
							break;
						}
					}
					if (!ResolvedNeighbor) { continue; }

					// Copy the neighbor's pin type (preserves container kind: Array / Set / Map
					// / Single). This is the same propagation pattern UK2Node_CallArrayFunction
					// uses internally.
					WildPin->PinType = ResolvedNeighbor->PinType;
					if (UEdGraphNode* Owner = WildPin->GetOwningNodeUnchecked(); IsValid(Owner))
					{
						// NotifyPinConnectionListChanged is K2-specific; UEdGraphNode base class
						// doesn't expose it. Cast first; if not a K2Node, fall back to graph-level
						// notification which UK2Node's override also routes through.
						if (UK2Node* K2Owner = Cast<UK2Node>(Owner); IsValid(K2Owner))
						{
							K2Owner->NotifyPinConnectionListChanged(WildPin);
						}
						else if (UEdGraph* OwnerGraph = Owner->GetGraph(); IsValid(OwnerGraph))
						{
							OwnerGraph->NotifyGraphChanged();
						}
					}
					bChangedAny = true;
				}
			}

			if (!bChangedAny)
			{
				break;
			}
		}
	}

	void PromoteWildcardContainerElementPin(UEdGraphPin* ElementPin, const FEdGraphPinType& InferredType)
	{
		if (!ElementPin)
		{
			return;
		}

		// The pin's own container kind is authoritative -- element pins are always
		// EPinContainerType::None and the inferred type never carries a meaningful one.
		const EPinContainerType PreservedContainer = ElementPin->PinType.ContainerType;
		ElementPin->PinType = InferredType;
		ElementPin->PinType.ContainerType = PreservedContainer;

		UEdGraphNode* Node = ElementPin->GetOwningNodeUnchecked();
		if (!IsValid(Node))
		{
			return;
		}

		if (UK2Node_MakeContainer* MakeNode = Cast<UK2Node_MakeContainer>(Node); IsValid(MakeNode))
		{
			// Mirror what UK2Node_MakeContainer::NotifyPinConnectionListChanged does for the
			// linked case, invoked directly because there is no link to trigger the hook.
			TArray<UEdGraphPin*> KeyPins;
			TArray<UEdGraphPin*> ValuePins;
			MakeNode->GetKeyAndValuePins(KeyPins, ValuePins);
			const bool bIsValuePin = ValuePins.Contains(ElementPin);

			if (UEdGraphPin* OutputPin = MakeNode->GetOutputPin())
			{
				if (bIsValuePin)
				{
					// Map value half lives in PinValueType, not the pin type proper.
					if (OutputPin->PinType.PinValueType.TerminalCategory == UEdGraphSchema_K2::PC_Wildcard)
					{
						OutputPin->PinType.PinValueType.TerminalCategory = InferredType.PinCategory;
						OutputPin->PinType.PinValueType.TerminalSubCategory = InferredType.PinSubCategory;
						OutputPin->PinType.PinValueType.TerminalSubCategoryObject = InferredType.PinSubCategoryObject;
					}
				}
				else if (OutputPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Wildcard)
				{
					// Preserve the output's container kind (Array / Set / Map).
					OutputPin->PinType.PinCategory = InferredType.PinCategory;
					OutputPin->PinType.PinSubCategory = InferredType.PinSubCategory;
					OutputPin->PinType.PinSubCategoryObject = InferredType.PinSubCategoryObject;
				}
			}

			// Sibling propagation: other still-wildcard element pins of the same half
			// (key vs value) take the promoted type too. New behavior -- the engine only
			// promotes siblings in response to a real connection.
			const TArray<UEdGraphPin*>& Siblings = bIsValuePin ? ValuePins : KeyPins;
			for (UEdGraphPin* Sibling : Siblings)
			{
				if (!Sibling || Sibling == ElementPin) { continue; }
				if (Sibling->PinType.PinCategory != UEdGraphSchema_K2::PC_Wildcard) { continue; }
				const EPinContainerType SiblingContainer = Sibling->PinType.ContainerType;
				Sibling->PinType = InferredType;
				Sibling->PinType.ContainerType = SiblingContainer;
			}
		}

		// Deliberately NOT calling NotifyPinConnectionListChanged here. On an unlinked
		// pin it takes the reset-to-wildcard branch (K2Node_MakeContainer.cpp:265), whose
		// PinsInUse check is driven by DoesDefaultValueMatchAutogenerated(). The caller
		// has not written the literal yet at this point, so the default still matches and
		// the node would undo the promotion we just made. The pin types above are set
		// directly, which is what that hook would have done for a linked pin anyway.
		if (UEdGraph* OwnerGraph = Node->GetGraph(); IsValid(OwnerGraph))
		{
			OwnerGraph->NotifyGraphChanged();
		}

		// The now-typed pin may sit on a node with other links elsewhere in the graph
		// that still need forward propagation.
		PropagateWildcardTypesViaLinks({Node});
	}

	namespace ClaireonBlueprintHelpers_FindNodesByTitle_Internal
	{
		/** Return the first line of a node title with trailing whitespace stripped.
		 *  CallFunction nodes auto-append a "Target is X" subtitle separated by \n on
		 *  ENodeTitleType::ListView; callers typically only know/type the first line. */
		FString FirstLineTrimmed(const FString& In)
		{
			int32 NewlineIdx = INDEX_NONE;
			if (!In.FindChar(TEXT('\n'), NewlineIdx))
			{
				NewlineIdx = INDEX_NONE;
			}
			int32 CarriageIdx = INDEX_NONE;
			if (In.FindChar(TEXT('\r'), CarriageIdx))
			{
				if (NewlineIdx == INDEX_NONE || CarriageIdx < NewlineIdx)
				{
					NewlineIdx = CarriageIdx;
				}
			}
			FString First = (NewlineIdx == INDEX_NONE) ? In : In.Left(NewlineIdx);
			return First.TrimStartAndEnd();
		}

		/** Fold a title for suggestion matching. NameToDisplayString only ever inserts spaces
		 *  at case/digit boundaries, turns '_' into a space, and adjusts case, so stripping the
		 *  spaces and comparing case-insensitively nets out to: ignore spaces, underscores, and
		 *  case. That covers the friendly-vs-raw rendering split ("Print String"/"PrintString")
		 *  without depending on which side produced which form. */
		FString NormalizedForMatch(const FString& In)
		{
			return FName::NameToDisplayString(In, /*bIsBool=*/false).Replace(TEXT(" "), TEXT(""));
		}

		/** "<guid> (<class>)" -- the minimum a caller needs to re-issue the call by GUID. */
		FString DescribeNodeForHint(const UEdGraphNode* Node)
		{
			return FString::Printf(TEXT("%s (%s)"),
				*Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
				*Node->GetClass()->GetName());
		}

		/** Join node descriptions, capping the list so a large graph cannot flood the response. */
		FString JoinNodeDescriptions(const TArray<UEdGraphNode*>& Nodes, int32 MaxListed = 10)
		{
			TArray<FString> Parts;
			const int32 Listed = FMath::Min(Nodes.Num(), MaxListed);
			for (int32 Idx = 0; Idx < Listed; ++Idx)
			{
				Parts.Add(DescribeNodeForHint(Nodes[Idx]));
			}
			FString Joined = FString::Join(Parts, TEXT(", "));
			if (Nodes.Num() > Listed)
			{
				Joined += FString::Printf(TEXT(", ... and %d more"), Nodes.Num() - Listed);
			}
			return Joined;
		}
	}

	TArray<UEdGraphNode*> FindNodesByTitle(UEdGraph* Graph, const FString& NodeTitle, bool bExactMatch)
	{
		using namespace ClaireonBlueprintHelpers_FindNodesByTitle_Internal;
		TArray<UEdGraphNode*> MatchingNodes;

		if (!IsValid(Graph) || NodeTitle.IsEmpty())
		{
			return MatchingNodes;
		}

		// Normalize the search input (callers may pass either the first line as the editor
		// shows it on hover, or the full multi-line title).
		const FString SearchTitle = NodeTitle;
		const FString SearchTitleFirst = FirstLineTrimmed(NodeTitle);

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}

			const FString CurrentTitle = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			const FString CurrentTitleFirst = FirstLineTrimmed(CurrentTitle);

			if (bExactMatch)
			{
				// Match either the full multi-line form (legacy callers) or the first
				// line only (most callers know the visible name without subtitle).
				if (CurrentTitle.Equals(SearchTitle, ESearchCase::IgnoreCase)
					|| CurrentTitleFirst.Equals(SearchTitleFirst, ESearchCase::IgnoreCase))
				{
					MatchingNodes.Add(Node);
				}
			}
			else
			{
				if (CurrentTitle.Contains(SearchTitle, ESearchCase::IgnoreCase)
					|| CurrentTitleFirst.Contains(SearchTitleFirst, ESearchCase::IgnoreCase))
				{
					MatchingNodes.Add(Node);
				}
			}
		}
		// Friendly-name settings change title spacing. Normalize after an exact miss, retaining ambiguous matches.
		if (bExactMatch && MatchingNodes.Num() == 0)
		{
			MatchingNodes = FindNodesByNormalizedTitle(Graph, NodeTitle, /*bAllowPartial=*/false);
		}
		return MatchingNodes;
	}

	TArray<UEdGraphNode*> FindNodesByNormalizedTitle(UEdGraph* Graph, const FString& NodeTitle, bool bAllowPartial)
	{
		using namespace ClaireonBlueprintHelpers_FindNodesByTitle_Internal;
		TArray<UEdGraphNode*> MatchingNodes;

		if (!IsValid(Graph) || NodeTitle.IsEmpty())
		{
			return MatchingNodes;
		}

		const FString SearchNormalized = NormalizedForMatch(FirstLineTrimmed(NodeTitle));
		if (SearchNormalized.IsEmpty())
		{
			return MatchingNodes;
		}

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}

			const FString CurrentNormalized =
				NormalizedForMatch(FirstLineTrimmed(Node->GetNodeTitle(ENodeTitleType::ListView).ToString()));

			const bool bMatches = bAllowPartial
				? CurrentNormalized.Contains(SearchNormalized, ESearchCase::IgnoreCase)
				: CurrentNormalized.Equals(SearchNormalized, ESearchCase::IgnoreCase);

			if (bMatches)
			{
				MatchingNodes.Add(Node);
			}
		}

		return MatchingNodes;
	}

	FString FormatTitleSuggestions(UEdGraph* Graph, const FString& RequestedTitle, const FString& DisambiguationParam)
	{
		using namespace ClaireonBlueprintHelpers_FindNodesByTitle_Internal;

		// Prefer whole-title candidates; only fall back to the partial pass (which catches
		// truncations such as "PrintStr") when nothing folds to the same normalized title.
		TArray<UEdGraphNode*> Candidates = FindNodesByNormalizedTitle(Graph, RequestedTitle, /*bAllowPartial=*/false);
		bool bPartial = false;
		if (Candidates.Num() == 0)
		{
			Candidates = FindNodesByNormalizedTitle(Graph, RequestedTitle, /*bAllowPartial=*/true);
			bPartial = Candidates.Num() > 0;
		}

		if (Candidates.Num() == 0)
		{
			return FString();
		}

		// Group by the title as rendered, so six identical "Branch" nodes read as one
		// suggestion with six GUIDs rather than six near-identical lines.
		TArray<FString> OrderedTitles;
		TMap<FString, TArray<UEdGraphNode*>> ByTitle;
		for (UEdGraphNode* Node : Candidates)
		{
			const FString Title = FirstLineTrimmed(Node->GetNodeTitle(ENodeTitleType::ListView).ToString());
			TArray<UEdGraphNode*>& Bucket = ByTitle.FindOrAdd(Title);
			if (Bucket.Num() == 0)
			{
				OrderedTitles.Add(Title);
			}
			Bucket.Add(Node);
		}

		TArray<FString> Suggestions;
		for (const FString& Title : OrderedTitles)
		{
			Suggestions.Add(FString::Printf(TEXT("\"%s\" [%s]"),
				*Title, *JoinNodeDescriptions(ByTitle[Title])));
		}

		return FString::Printf(
			TEXT("Did you mean: %s? (Suggestions match %s ignoring spaces, underscores, and case -- ")
			TEXT("re-issue with the exact title above, or pass %s.)"),
			*FString::Join(Suggestions, TEXT(", ")),
			bPartial ? TEXT("partially") : TEXT("exactly"),
			*DisambiguationParam);
	}

	FString FormatAmbiguousNodeMatch(
		UEdGraph* Graph,
		const FString& LookupKind,
		const FString& RequestedValue,
		const TArray<UEdGraphNode*>& Matches,
		const FString& Remedy)
	{
		using namespace ClaireonBlueprintHelpers_FindNodesByTitle_Internal;

		const FString GraphName = IsValid(Graph) ? Graph->GetName() : TEXT("<null>");

		return FString::Printf(
			TEXT("Ambiguous %s '%s' in graph '%s' -- %d matches: %s. %s"),
			*LookupKind, *RequestedValue, *GraphName, Matches.Num(),
			*JoinNodeDescriptions(Matches), *Remedy);
	}

	FString FormatTitleMatchFailure(
		UEdGraph* Graph,
		const FString& RequestedTitle,
		const TArray<UEdGraphNode*>& ExactMatches,
		const FString& DisambiguationParam)
	{
		using namespace ClaireonBlueprintHelpers_FindNodesByTitle_Internal;

		const FString GraphName = IsValid(Graph) ? Graph->GetName() : TEXT("<null>");

		if (ExactMatches.Num() > 1)
		{
			return FormatAmbiguousNodeMatch(
				Graph, TEXT("node title"), RequestedTitle, ExactMatches,
				FString::Printf(TEXT("Pass %s to disambiguate."), *DisambiguationParam));
		}

		// No exact match. Offer normalized candidates before dumping the whole graph: the
		// caller almost always wants the one title it got the spacing or case wrong on.
		const FString Suggestions = FormatTitleSuggestions(Graph, RequestedTitle, DisambiguationParam);
		if (!Suggestions.IsEmpty())
		{
			return FString::Printf(TEXT("Node not found by title '%s' in graph '%s'. %s"),
				*RequestedTitle, *GraphName, *Suggestions);
		}

		return FString::Printf(TEXT("Node not found by title '%s' in graph '%s'. %s"),
			*RequestedTitle, *GraphName, *FormatAvailableNodes(Graph));
	}

	UEdGraph* FindGraphByName(UBlueprint* Blueprint, const FString& GraphName)
	{
		if (!IsValid(Blueprint))
		{
			return nullptr;
		}

		// Check UbergraphPages (EventGraph)
		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (IsValid(Graph) && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}

		// Check FunctionGraphs
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (IsValid(Graph) && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}

		// Check MacroGraphs
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (IsValid(Graph) && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}

		// Check DelegateSignatureGraphs
		for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
		{
			if (IsValid(Graph) && Graph->GetName() == GraphName)
			{
				return Graph;
			}
		}

		// Composite (collapsed-graph) subgraphs live in their parent graph's
		// SubGraphs array, recursively. GetAllGraphs collects them all.
		{
			TArray<UEdGraph*> AllGraphs;
			Blueprint->GetAllGraphs(AllGraphs);
			for (UEdGraph* Graph : AllGraphs)
			{
				if (IsValid(Graph) && Graph->GetName() == GraphName)
				{
					return Graph;
				}
			}
		}

		return nullptr;
	}

	const TCHAR* GraphKindToWireString(EClaireonGraphKind Kind)
	{
		// Omit default so new graph kinds require a wire spelling.
		switch (Kind)
		{
		case EClaireonGraphKind::Ubergraph:               return TEXT("ubergraph");
		case EClaireonGraphKind::Function:                return TEXT("function");
		case EClaireonGraphKind::Macro:                   return TEXT("macro");
		case EClaireonGraphKind::DelegateSignature:       return TEXT("delegate_signature");
		case EClaireonGraphKind::InterfaceImplementation: return TEXT("interface_implementation");
		case EClaireonGraphKind::Extension:               return TEXT("extension");
		}
		return TEXT("ubergraph");
	}

	bool ParseGraphKind(const FString& Wire, EClaireonGraphKind& OutKind)
	{
		static const EClaireonGraphKind AllKinds[] = {
			EClaireonGraphKind::Ubergraph,
			EClaireonGraphKind::Function,
			EClaireonGraphKind::Macro,
			EClaireonGraphKind::DelegateSignature,
			EClaireonGraphKind::InterfaceImplementation,
			EClaireonGraphKind::Extension,
		};

		for (const EClaireonGraphKind Kind : AllKinds)
		{
			if (Wire.Equals(GraphKindToWireString(Kind), ESearchCase::IgnoreCase))
			{
				OutKind = Kind;
				return true;
			}
		}
		return false;
	}

	TArray<FGraphEnumerationEntry> EnumerateTopLevelGraphs(UBlueprint* Blueprint)
	{
		TArray<FGraphEnumerationEntry> Result;
		if (!IsValid(Blueprint))
		{
			return Result;
		}

		for (UEdGraph* Graph : Blueprint->UbergraphPages)
		{
			if (IsValid(Graph))
			{
				Result.Add(FGraphEnumerationEntry{ Graph, EClaireonGraphKind::Ubergraph, FString() });
			}
		}
		for (UEdGraph* Graph : Blueprint->FunctionGraphs)
		{
			if (IsValid(Graph))
			{
				Result.Add(FGraphEnumerationEntry{ Graph, EClaireonGraphKind::Function, FString() });
			}
		}
		for (UEdGraph* Graph : Blueprint->MacroGraphs)
		{
			if (IsValid(Graph))
			{
				Result.Add(FGraphEnumerationEntry{ Graph, EClaireonGraphKind::Macro, FString() });
			}
		}
		for (UEdGraph* Graph : Blueprint->DelegateSignatureGraphs)
		{
			if (IsValid(Graph))
			{
				Result.Add(FGraphEnumerationEntry{ Graph, EClaireonGraphKind::DelegateSignature, FString() });
			}
		}
		for (const FBPInterfaceDescription& InterfaceDesc : Blueprint->ImplementedInterfaces)
		{
			const FString InterfacePath = IsValid(InterfaceDesc.Interface) ? InterfaceDesc.Interface->GetPathName() : FString();
			for (UEdGraph* Graph : InterfaceDesc.Graphs)
			{
				if (IsValid(Graph))
				{
					Result.Add(FGraphEnumerationEntry{ Graph, EClaireonGraphKind::InterfaceImplementation, InterfacePath });
				}
			}
		}

		// Enumerate extension-owned top-level graphs without recursing into nested composites.
		for (const UBlueprintExtension* Extension : Blueprint->GetExtensions())
		{
			if (!IsValid(Extension))
			{
				continue;
			}
			TArray<UEdGraph*> ExtensionGraphs;
			Extension->GetAllGraphs(ExtensionGraphs);
			for (UEdGraph* Graph : ExtensionGraphs)
			{
				if (IsValid(Graph))
				{
					Result.Add(FGraphEnumerationEntry{ Graph, EClaireonGraphKind::Extension, FString() });
				}
			}
		}

		return Result;
	}

	TArray<UEdGraphPin*> FindCompatiblePins(UEdGraphNode* Node, UEdGraphPin* Pin)
	{
		TArray<UEdGraphPin*> CompatiblePins;

		if (!IsValid(Node) || !Pin)
		{
			return CompatiblePins;
		}

		const UEdGraphSchema_K2* Schema = Cast<UEdGraphSchema_K2>(Node->GetSchema());
		if (!IsValid(Schema))
		{
			return CompatiblePins;
		}

		// Find pins with opposite direction
		EEdGraphPinDirection OppositeDirection = (Pin->Direction == EGPD_Input) ? EGPD_Output : EGPD_Input;

		for (UEdGraphPin* NodePin : Node->Pins)
		{
			if (NodePin && NodePin->Direction == OppositeDirection)
			{
				// Check if connection is possible
				FPinConnectionResponse Response = Schema->CanCreateConnection(Pin, NodePin);
				if (Response.Response == CONNECT_RESPONSE_MAKE || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_A || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_B || Response.Response == CONNECT_RESPONSE_BREAK_OTHERS_AB)
				{
					CompatiblePins.Add(NodePin);
				}
			}
		}

		return CompatiblePins;
	}

	namespace ClaireonBlueprintHelpers_Private
	{
		// Returns true if TypeString begins with Prefix (case-insensitive) and either
		// ends immediately at Prefix.Len() or continues with characters (used for StartsWith tests).
		bool StartsWithI(const FString& TypeString, const TCHAR* Prefix)
		{
			return TypeString.StartsWith(Prefix, ESearchCase::IgnoreCase);
		}

		// Scan TypeString for a matching '>' starting at OpenIndex (which must point at '<').
		// Returns the index of the matching '>' with balanced <...> nesting, or INDEX_NONE on
		// unbalanced input.
		int32 FindMatchingAngle(const FString& TypeString, int32 OpenIndex)
		{
			if (!TypeString.IsValidIndex(OpenIndex) || TypeString[OpenIndex] != TEXT('<'))
			{
				return INDEX_NONE;
			}
			int32 Depth = 0;
			for (int32 i = OpenIndex; i < TypeString.Len(); ++i)
			{
				const TCHAR C = TypeString[i];
				if (C == TEXT('<'))
				{
					++Depth;
				}
				else if (C == TEXT('>'))
				{
					--Depth;
					if (Depth == 0)
					{
						return i;
					}
				}
			}
			return INDEX_NONE;
		}

		// Split Inner at the first top-level comma (depth 0). Nested generics are ignored.
		// Returns true iff exactly one top-level comma splits Inner into two non-empty parts.
		bool SplitMapInnerAtTopLevelComma(const FString& Inner, FString& OutKey, FString& OutValue, FString& OutError)
		{
			int32 Depth = 0;
			int32 CommaIndex = INDEX_NONE;
			int32 TopLevelCommaCount = 0;
			for (int32 i = 0; i < Inner.Len(); ++i)
			{
				const TCHAR C = Inner[i];
				if (C == TEXT('<'))
				{
					++Depth;
				}
				else if (C == TEXT('>'))
				{
					--Depth;
				}
				else if (C == TEXT(',') && Depth == 0)
				{
					if (TopLevelCommaCount == 0)
					{
						CommaIndex = i;
					}
					++TopLevelCommaCount;
				}
			}

			if (TopLevelCommaCount == 0)
			{
				OutError = TEXT("Map<K,V> requires exactly two type parameters separated by a top-level comma");
				return false;
			}
			if (TopLevelCommaCount > 1)
			{
				OutError = FString::Printf(TEXT("Map<K,V> expects exactly two type parameters; got %d top-level parts"), TopLevelCommaCount + 1);
				return false;
			}

			OutKey = Inner.Mid(0, CommaIndex).TrimStartAndEnd();
			OutValue = Inner.Mid(CommaIndex + 1).TrimStartAndEnd();
			if (OutKey.IsEmpty() || OutValue.IsEmpty())
			{
				OutError = TEXT("Map<K,V> has an empty key or value part");
				return false;
			}
			return true;
		}

		// Resolve a UFunction from a fully-qualified path such as
		// "/Script/MyModule.MyClass.MyDelegate__DelegateSignature" or
		// "/Script/X.Y__DelegateSignature".
		UFunction* ResolveSignatureFunction(const FString& Path)
		{
			if (Path.IsEmpty())
			{
				return nullptr;
			}
			// FindObject supports module-scoped paths with dotted segments. Try direct first.
			UFunction* Fn = FindObject<UFunction>(nullptr, *Path);
			if (IsValid(Fn))
			{
				return Fn;
			}
			// Fall back: LoadObject resolves redirectors + deferred loads.
			return LoadObject<UFunction>(nullptr, *Path);
		}

		void SetDelegateSignature(FEdGraphPinType& PinType, UFunction* SignatureFn)
		{
			if (!IsValid(SignatureFn))
			{
				return;
			}
			FMemberReference::FillSimpleMemberReference<UFunction>(SignatureFn, PinType.PinSubCategoryMemberReference);
		}

		/**
		 * Return a signature path accepted by ResolveSignatureFunction.
		 * An empty result means the reference cannot be resolved without an owning scope.
		 */
		FString GetDelegateSignaturePath(const FEdGraphPinType& PinType)
		{
			const FSimpleMemberReference& Ref = PinType.PinSubCategoryMemberReference;
			if (Ref.MemberName.IsNone())
			{
				return FString();
			}

			// MemberParent can be a class or a package for a globally declared native delegate.
			const UObject* Parent = Ref.MemberParent;
			if (!IsValid(Parent))
			{
				return FString();
			}
			return FString::Printf(TEXT("%s.%s"), *Parent->GetPathName(), *Ref.MemberName.ToString());
		}

		bool IsDelegateCategory(const FName& Category)
		{
			return Category == UEdGraphSchema_K2::PC_Delegate
				|| Category == UEdGraphSchema_K2::PC_MCDelegate;
		}

		// Parse a SoftClass<X> / SoftObject<X> generic-angle body. Inner is the contents
		// between the angles (already stripped).
		FParseVariableTypeResult ParseSoftRefClass(const FString& Inner, bool bSoftClass)
		{
			FParseVariableTypeResult Result;
			const FString Trimmed = Inner.TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				Result.Error = bSoftClass
					? TEXT("SoftClass<Class> requires a class name inside the angle brackets")
					: TEXT("SoftObject<Class> requires a class name inside the angle brackets");
				return Result;
			}
			ClaireonNameResolver::FNameResolveResult Resolve;
			UClass* Cls = ClaireonNameResolver::ResolveClassName(Trimmed, nullptr, Resolve);
			if (!IsValid(Cls))
			{
				Result.Error = FString::Printf(TEXT("Could not resolve class '%s' for soft reference: %s"), *Trimmed, *Resolve.Error);
				return Result;
			}
			Result.PinType.PinCategory = bSoftClass ? UEdGraphSchema_K2::PC_SoftClass : UEdGraphSchema_K2::PC_SoftObject;
			Result.PinType.PinSubCategoryObject = Cls;
			Result.ResolutionNote = Resolve.ResolutionNote;
			Result.bSucceeded = true;
			return Result;
		}

		// Parse `softclass:/Game/.../X.X_C` or `softobject:/Game/.../X.X` prefix form.
		FParseVariableTypeResult ParseSoftRefPath(const FString& PathStr, bool bSoftClass)
		{
			FParseVariableTypeResult Result;
			const FString Trimmed = PathStr.TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				Result.Error = TEXT("softclass: / softobject: prefix requires an object path");
				return Result;
			}
			UClass* Cls = FindObject<UClass>(nullptr, *Trimmed);
			if (!IsValid(Cls))
			{
				Cls = LoadObject<UClass>(nullptr, *Trimmed);
			}
			if (!IsValid(Cls))
			{
				Result.Error = FString::Printf(TEXT("Could not resolve class path '%s' for soft reference"), *Trimmed);
				return Result;
			}
			Result.PinType.PinCategory = bSoftClass ? UEdGraphSchema_K2::PC_SoftClass : UEdGraphSchema_K2::PC_SoftObject;
			Result.PinType.PinSubCategoryObject = Cls;
			Result.bSucceeded = true;
			return Result;
		}

		// Parse `InstancedStruct<FMyStruct>` body. Inner is the contents between angles.
		FParseVariableTypeResult ParseInstancedStructGeneric(const FString& Inner)
		{
			FParseVariableTypeResult Result;
			const FString Trimmed = Inner.TrimStartAndEnd();
			if (Trimmed.IsEmpty())
			{
				// Bare InstancedStruct<> is valid -- meta info points at FInstancedStruct only.
				Result.PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				Result.PinType.PinSubCategoryObject = FInstancedStruct::StaticStruct();
				Result.bSucceeded = true;
				return Result;
			}
			ClaireonNameResolver::FNameResolveResult Resolve;
			UScriptStruct* InnerStruct = ClaireonNameResolver::ResolveStructName(Trimmed, Resolve);
			if (!IsValid(InnerStruct))
			{
				Result.Error = FString::Printf(TEXT("Could not resolve struct '%s' for InstancedStruct<>: %s"), *Trimmed, *Resolve.Error);
				return Result;
			}
			Result.PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
			Result.PinType.PinSubCategoryObject = FInstancedStruct::StaticStruct();
			// PinSubCategoryObject is fixed to FInstancedStruct; InnerStruct is the base-struct
			// hint (written as BaseStruct metadata by downstream ApplyVariableProperties callers).
			// We carry it forward via the resolution note so callers surface the intent.
			Result.ResolutionNote = Resolve.ResolutionNote.IsEmpty()
				? FString::Printf(TEXT("InstancedStruct base struct: %s"), *InnerStruct->GetName())
				: Resolve.ResolutionNote;
			Result.bSucceeded = true;
			return Result;
		}
	} // namespace
	using namespace ClaireonBlueprintHelpers_Private;

	FParseVariableTypeResult ParseVariableTypeChecked(const FString& TypeString)
	{
		FParseVariableTypeResult Result;
		FEdGraphPinType& PinType = Result.PinType;

		if (TypeString.IsEmpty())
		{
			Result.Error = TEXT("variable_type is empty");
			return Result;
		}

		// --- Simple scalars (preserve legacy case-sensitive behavior for these keywords).
		if (TypeString == TEXT("float") || TypeString.Equals(TEXT("Float"), ESearchCase::IgnoreCase))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Float;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("double") || TypeString.Equals(TEXT("Double"), ESearchCase::IgnoreCase))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Real;
			PinType.PinSubCategory = UEdGraphSchema_K2::PC_Double;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("int") || TypeString == TEXT("int32") || TypeString.Equals(TEXT("Integer"), ESearchCase::IgnoreCase))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Int;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("int64") || TypeString.Equals(TEXT("Integer64"), ESearchCase::IgnoreCase))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Int64;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("byte") || TypeString.Equals(TEXT("Byte"), ESearchCase::IgnoreCase))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("bool") || TypeString.Equals(TEXT("Boolean"), ESearchCase::IgnoreCase) || TypeString.Equals(TEXT("Bool"), ESearchCase::IgnoreCase))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("string") || TypeString.Equals(TEXT("String"), ESearchCase::IgnoreCase) || TypeString == TEXT("FString"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_String;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("name") || TypeString.Equals(TEXT("Name"), ESearchCase::IgnoreCase) || TypeString == TEXT("FName"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Name;
			Result.bSucceeded = true;
			return Result;
		}
		if (TypeString == TEXT("text") || TypeString.Equals(TEXT("Text"), ESearchCase::IgnoreCase) || TypeString == TEXT("FText"))
		{
			PinType.PinCategory = UEdGraphSchema_K2::PC_Text;
			Result.bSucceeded = true;
			return Result;
		}

		// --- Expanded named types (D4/D5).
		// Delegate family: flat-string form is an error without signature_function (per D4).
		{
			const FString LowerName = TypeString.ToLower();
			if (LowerName == TEXT("mcdelegate") || LowerName == TEXT("dispatcher") || LowerName == TEXT("multicastinlinedelegate") || LowerName == TEXT("multicastdelegate"))
			{
				Result.Error = TEXT("multicast delegate requires variable_type_spec.signature_function (caller must supply the UFunction signature path)");
				return Result;
			}
			if (LowerName == TEXT("delegate") || LowerName == TEXT("singledelegate"))
			{
				Result.Error = TEXT("delegate requires variable_type_spec.signature_function (caller must supply the UFunction signature path)");
				return Result;
			}
			if (LowerName == TEXT("instancedstruct"))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = FInstancedStruct::StaticStruct();
				Result.bSucceeded = true;
				return Result;
			}
			if (LowerName == TEXT("gameplaytag"))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = FGameplayTag::StaticStruct();
				Result.bSucceeded = true;
				return Result;
			}
			if (LowerName == TEXT("gameplaytagcontainer"))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = FGameplayTagContainer::StaticStruct();
				Result.bSucceeded = true;
				return Result;
			}
		}

		// --- Prefix forms: softclass:/path, softobject:/path
		if (StartsWithI(TypeString, TEXT("softclass:")))
		{
			return ParseSoftRefPath(TypeString.Mid(10), /*bSoftClass=*/true);
		}
		if (StartsWithI(TypeString, TEXT("softobject:")))
		{
			return ParseSoftRefPath(TypeString.Mid(11), /*bSoftClass=*/false);
		}

		// --- Generic angle forms: Array<T>, Set<T>, Map<K,V>, SoftClass<X>, SoftObject<X>,
		// InstancedStruct<X>, SoftClassReference<X>, SoftObjectReference<X>.
		auto TryStripAngleForm = [&TypeString](const TCHAR* Keyword, int32 KeywordLen, FString& OutInner) -> bool
		{
			if (TypeString.Len() < KeywordLen + 2) return false;
			if (TypeString.Left(KeywordLen).Equals(Keyword, ESearchCase::IgnoreCase) &&
				TypeString[KeywordLen] == TEXT('<') &&
				TypeString.EndsWith(TEXT(">")))
			{
				// Validate angle balance for the whole string.
				const int32 Close = FindMatchingAngle(TypeString, KeywordLen);
				if (Close != TypeString.Len() - 1) return false;
				OutInner = TypeString.Mid(KeywordLen + 1, Close - KeywordLen - 1);
				return true;
			}
			return false;
		};

		FString GenericInner;
		if (TryStripAngleForm(TEXT("Array"), 5, GenericInner))
		{
			FParseVariableTypeResult Inner = ParseVariableTypeChecked(GenericInner.TrimStartAndEnd());
			if (!Inner.bSucceeded)
			{
				Result.Error = FString::Printf(TEXT("Array<T> element parse failed: %s"), *Inner.Error);
				return Result;
			}
			PinType = Inner.PinType;
			PinType.ContainerType = EPinContainerType::Array;
			Result.ResolutionNote = Inner.ResolutionNote;
			Result.bSucceeded = true;
			return Result;
		}
		if (TryStripAngleForm(TEXT("Set"), 3, GenericInner))
		{
			FParseVariableTypeResult Inner = ParseVariableTypeChecked(GenericInner.TrimStartAndEnd());
			if (!Inner.bSucceeded)
			{
				Result.Error = FString::Printf(TEXT("Set<T> element parse failed: %s"), *Inner.Error);
				return Result;
			}
			PinType = Inner.PinType;
			PinType.ContainerType = EPinContainerType::Set;
			Result.ResolutionNote = Inner.ResolutionNote;
			Result.bSucceeded = true;
			return Result;
		}
		if (TryStripAngleForm(TEXT("Map"), 3, GenericInner))
		{
			FString KeyStr, ValueStr, SplitErr;
			if (!SplitMapInnerAtTopLevelComma(GenericInner, KeyStr, ValueStr, SplitErr))
			{
				Result.Error = FString::Printf(TEXT("Map<K,V> parse failed: %s"), *SplitErr);
				return Result;
			}

			FParseVariableTypeResult Key = ParseVariableTypeChecked(KeyStr);
			if (!Key.bSucceeded)
			{
				Result.Error = FString::Printf(TEXT("Map<K,V> key parse failed: key:%s"), *Key.Error);
				return Result;
			}
			FParseVariableTypeResult Value = ParseVariableTypeChecked(ValueStr);
			if (!Value.bSucceeded)
			{
				Result.Error = FString::Printf(TEXT("Map<K,V> value parse failed: value:%s"), *Value.Error);
				return Result;
			}

			PinType.ContainerType = EPinContainerType::Map;
			PinType.PinCategory = Key.PinType.PinCategory;
			PinType.PinSubCategory = Key.PinType.PinSubCategory;
			PinType.PinSubCategoryObject = Key.PinType.PinSubCategoryObject;
			PinType.PinValueType.TerminalCategory = Value.PinType.PinCategory;
			PinType.PinValueType.TerminalSubCategory = Value.PinType.PinSubCategory;
			PinType.PinValueType.TerminalSubCategoryObject = Value.PinType.PinSubCategoryObject;
			Result.bSucceeded = true;
			return Result;
		}
		// Delegate angle forms carry the signature required for read/write round trips.
		{
			static const TCHAR* MultiAliases[] = {
				TEXT("MCDelegate"), TEXT("Dispatcher"), TEXT("MulticastDelegate"), TEXT("MulticastInlineDelegate")};
			static const TCHAR* SingleAliases[] = {TEXT("Delegate"), TEXT("SingleDelegate")};

			bool bIsMulti = false;
			bool bMatched = false;
			for (const TCHAR* Alias : MultiAliases)
			{
				if (TryStripAngleForm(Alias, FCString::Strlen(Alias), GenericInner))
				{
					bIsMulti = true;
					bMatched = true;
					break;
				}
			}
			if (!bMatched)
			{
				for (const TCHAR* Alias : SingleAliases)
				{
					if (TryStripAngleForm(Alias, FCString::Strlen(Alias), GenericInner))
					{
						bMatched = true;
						break;
					}
				}
			}

			if (bMatched)
			{
				const FString SignaturePath = GenericInner.TrimStartAndEnd();
				if (SignaturePath.IsEmpty())
				{
					Result.Error = TEXT("Delegate<Signature> requires a UFunction signature path inside the angle brackets, e.g. MCDelegate</Script/MyModule.MyClass.MyEvent__DelegateSignature>");
					return Result;
				}
				UFunction* SigFn = ResolveSignatureFunction(SignaturePath);
				if (!IsValid(SigFn))
				{
					Result.Error = FString::Printf(
						TEXT("signature_function '%s' could not be resolved to a UFunction"), *SignaturePath);
					return Result;
				}
				PinType.PinCategory = bIsMulti ? UEdGraphSchema_K2::PC_MCDelegate : UEdGraphSchema_K2::PC_Delegate;
				SetDelegateSignature(PinType, SigFn);
				Result.bSucceeded = true;
				return Result;
			}
		}
		if (TryStripAngleForm(TEXT("Class"), 5, GenericInner))
		{
			ClaireonNameResolver::FNameResolveResult ClassResult;
			UClass* MetaClass = ClaireonNameResolver::ResolveClassName(GenericInner.TrimStartAndEnd(), nullptr, ClassResult);
			if (!IsValid(MetaClass))
			{
				Result.Error = FString::Printf(TEXT("Class<X> parse failed: %s"), *ClassResult.Error);
				return Result;
			}
			PinType.PinCategory = UEdGraphSchema_K2::PC_Class;
			PinType.PinSubCategoryObject = MetaClass;
			Result.ResolutionNote = ClassResult.ResolutionNote;
			Result.bSucceeded = true;
			return Result;
		}
		if (TryStripAngleForm(TEXT("SoftClass"), 9, GenericInner) || TryStripAngleForm(TEXT("SoftClassReference"), 18, GenericInner))
		{
			return ParseSoftRefClass(GenericInner, /*bSoftClass=*/true);
		}
		if (TryStripAngleForm(TEXT("SoftObject"), 10, GenericInner) || TryStripAngleForm(TEXT("SoftObjectReference"), 19, GenericInner))
		{
			return ParseSoftRefClass(GenericInner, /*bSoftClass=*/false);
		}
		if (TryStripAngleForm(TEXT("InstancedStruct"), 15, GenericInner))
		{
			return ParseInstancedStructGeneric(GenericInner);
		}

		// --- Name-resolution fallbacks: UClass, UScriptStruct, UEnum (in order).
		{
			ClaireonNameResolver::FNameResolveResult ClassResult;
			UClass* Class = ClaireonNameResolver::ResolveClassName(TypeString, nullptr, ClassResult);
			if (IsValid(Class))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Object;
				PinType.PinSubCategoryObject = Class;
				Result.ResolutionNote = ClassResult.ResolutionNote;
				Result.bSucceeded = true;
				return Result;
			}
		}
		{
			ClaireonNameResolver::FNameResolveResult StructResult;
			UScriptStruct* Struct = ClaireonNameResolver::ResolveStructName(TypeString, StructResult);
			if (IsValid(Struct))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				PinType.PinSubCategoryObject = Struct;
				Result.ResolutionNote = StructResult.ResolutionNote;
				Result.bSucceeded = true;
				return Result;
			}
		}
		{
			ClaireonNameResolver::FNameResolveResult EnumResult;
			UEnum* Enum = ClaireonNameResolver::ResolveEnumName(TypeString, EnumResult);
			if (IsValid(Enum))
			{
				PinType.PinCategory = UEdGraphSchema_K2::PC_Byte;
				PinType.PinSubCategoryObject = Enum;
				Result.ResolutionNote = EnumResult.ResolutionNote;
				Result.bSucceeded = true;
				return Result;
			}
		}

		Result.Error = FString::Printf(TEXT("Unknown variable type '%s' (no matching keyword, class, struct, or enum)"), *TypeString);
		return Result;
	}

	FParseVariableTypeResult ParseVariableTypeSpec(const TSharedPtr<FJsonObject>& Spec)
	{
		FParseVariableTypeResult Result;
		if (!Spec.IsValid())
		{
			Result.Error = TEXT("variable_type_spec is missing or not an object");
			return Result;
		}

		FString Base;
		if (!Spec->TryGetStringField(TEXT("base"), Base) || Base.IsEmpty())
		{
			Result.Error = TEXT("variable_type_spec.base is required");
			return Result;
		}

		const FString LowerBase = Base.ToLower();
		const bool bMultiDelegate = (LowerBase == TEXT("mcdelegate") || LowerBase == TEXT("dispatcher") || LowerBase == TEXT("multicastinlinedelegate") || LowerBase == TEXT("multicastdelegate"));
		const bool bSingleDelegate = (LowerBase == TEXT("delegate") || LowerBase == TEXT("singledelegate"));

		FString SignatureFnPath;
		Spec->TryGetStringField(TEXT("signature_function"), SignatureFnPath);
		FString Subtype;
		Spec->TryGetStringField(TEXT("subtype"), Subtype);

		if (bMultiDelegate || bSingleDelegate)
		{
			if (SignatureFnPath.IsEmpty())
			{
				Result.Error = TEXT("delegate variable requires variable_type_spec.signature_function (caller must supply the UFunction signature path; the tool does not synthesize)");
				return Result;
			}
			UFunction* SigFn = ResolveSignatureFunction(SignatureFnPath);
			if (!IsValid(SigFn))
			{
				Result.Error = FString::Printf(TEXT("signature_function '%s' could not be resolved to a UFunction"), *SignatureFnPath);
				return Result;
			}
			Result.PinType.PinCategory = bMultiDelegate ? UEdGraphSchema_K2::PC_MCDelegate : UEdGraphSchema_K2::PC_Delegate;
			SetDelegateSignature(Result.PinType, SigFn);
			Result.bSucceeded = true;
			return Result;
		}

		// Soft-ref base with explicit subtype overrides the short-form.
		if (LowerBase == TEXT("softclass") || LowerBase == TEXT("softclassreference"))
		{
			if (Subtype.IsEmpty())
			{
				Result.Error = TEXT("softclass variable_type_spec requires 'subtype' (target class path or name)");
				return Result;
			}
			// Route through the class resolver.
			return ParseSoftRefClass(Subtype, /*bSoftClass=*/true);
		}
		if (LowerBase == TEXT("softobject") || LowerBase == TEXT("softobjectreference"))
		{
			if (Subtype.IsEmpty())
			{
				Result.Error = TEXT("softobject variable_type_spec requires 'subtype' (target class path or name)");
				return Result;
			}
			return ParseSoftRefClass(Subtype, /*bSoftClass=*/false);
		}
		if (LowerBase == TEXT("instancedstruct"))
		{
			if (Subtype.IsEmpty())
			{
				// Bare FInstancedStruct with no base-struct hint is legal.
				Result.PinType.PinCategory = UEdGraphSchema_K2::PC_Struct;
				Result.PinType.PinSubCategoryObject = FInstancedStruct::StaticStruct();
				Result.bSucceeded = true;
				return Result;
			}
			return ParseInstancedStructGeneric(Subtype);
		}

		// Otherwise fall through to the short-form parser with just base.
		return ParseVariableTypeChecked(Base);
	}

	FEdGraphPinType ParseVariableType(const FString& TypeString)
	{
		FParseVariableTypeResult Local = ParseVariableTypeChecked(TypeString);
		if (!Local.bSucceeded)
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[ParseVariableType] legacy-path parse failed for '%s': %s -- falling back to String."),
				*TypeString, *Local.Error);
			// The unchecked parser falls back to String for unknown types.
			FEdGraphPinType Fallback;
			Fallback.PinCategory = UEdGraphSchema_K2::PC_String;
			return Fallback;
		}
		return Local.PinType;
	}

	FString FormatVariableTypeString(const FEdGraphPinType& PinType)
	{
		// Format both the main type and map value terminal for replay.
		auto FormatBase = [](const FName& Category, const FName& SubCategory, const UObject* SubObj) -> FString
		{
			if (Category == UEdGraphSchema_K2::PC_Boolean)
			{
				return TEXT("Boolean");
			}
			if (Category == UEdGraphSchema_K2::PC_Byte)
			{
				// A byte pin with a bound UEnum is an enum variable; emit the enum name
				// so the round trip restores the enum binding instead of a raw byte.
				return IsValid(SubObj) ? SubObj->GetName() : TEXT("Byte");
			}
			if (Category == UEdGraphSchema_K2::PC_Int)
			{
				return TEXT("Int");
			}
			if (Category == UEdGraphSchema_K2::PC_Int64)
			{
				return TEXT("Int64");
			}
			if (Category == UEdGraphSchema_K2::PC_Real)
			{
				if (SubCategory == UEdGraphSchema_K2::PC_Float)
				{
					return TEXT("Float");
				}
				if (SubCategory == UEdGraphSchema_K2::PC_Double)
				{
					return TEXT("Double");
				}
				return TEXT("Real");
			}
			if (Category == UEdGraphSchema_K2::PC_String)
			{
				return TEXT("String");
			}
			if (Category == UEdGraphSchema_K2::PC_Name)
			{
				return TEXT("Name");
			}
			if (Category == UEdGraphSchema_K2::PC_Text)
			{
				return TEXT("Text");
			}
			if (Category == UEdGraphSchema_K2::PC_Object)
			{
				return IsValid(SubObj) ? SubObj->GetName() : TEXT("Object");
			}
			if (Category == UEdGraphSchema_K2::PC_Class)
			{
				return IsValid(SubObj) ? FString::Printf(TEXT("Class<%s>"), *SubObj->GetName()) : TEXT("Class");
			}
			if (Category == UEdGraphSchema_K2::PC_SoftObject)
			{
				return IsValid(SubObj) ? FString::Printf(TEXT("SoftObject<%s>"), *SubObj->GetName()) : TEXT("SoftObject");
			}
			if (Category == UEdGraphSchema_K2::PC_SoftClass)
			{
				return IsValid(SubObj) ? FString::Printf(TEXT("SoftClass<%s>"), *SubObj->GetName()) : TEXT("SoftClass");
			}
			if (Category == UEdGraphSchema_K2::PC_Struct || Category == UEdGraphSchema_K2::PC_Enum)
			{
				return IsValid(SubObj) ? SubObj->GetName() : Category.ToString();
			}
			return Category.ToString();
		};

		// Delegate signatures live on PinType, outside the terminal triple passed to FormatBase.
		// Blueprint variables cannot contain delegates in containers.
		if (IsDelegateCategory(PinType.PinCategory))
		{
			const FString Keyword = (PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate)
				? TEXT("MCDelegate")
				: TEXT("Delegate");
			const FString SignaturePath = GetDelegateSignaturePath(PinType);

			// An unresolved signature produces a bare keyword and type_round_trips=false.
			return SignaturePath.IsEmpty()
				? Keyword
				: FString::Printf(TEXT("%s<%s>"), *Keyword, *SignaturePath);
		}

		FString TypeStr;

		if (PinType.ContainerType == EPinContainerType::Array)
		{
			TypeStr += TEXT("Array<");
		}
		else if (PinType.ContainerType == EPinContainerType::Set)
		{
			TypeStr += TEXT("Set<");
		}
		else if (PinType.ContainerType == EPinContainerType::Map)
		{
			TypeStr += TEXT("Map<");
		}

		TypeStr += FormatBase(PinType.PinCategory, PinType.PinSubCategory, PinType.PinSubCategoryObject.Get());

		if (PinType.ContainerType == EPinContainerType::Map)
		{
			TypeStr += TEXT(",");
			TypeStr += FormatBase(PinType.PinValueType.TerminalCategory,
				PinType.PinValueType.TerminalSubCategory,
				PinType.PinValueType.TerminalSubCategoryObject.Get());
		}

		if (PinType.ContainerType != EPinContainerType::None)
		{
			TypeStr += TEXT(">");
		}

		return TypeStr;
	}

	bool IsDelegateVariableType(const FEdGraphPinType& PinType)
	{
		return IsDelegateCategory(PinType.PinCategory);
	}

	UFunction* ResolveDelegateSignatureFunction(const FEdGraphPinType& PinType,
		UBlueprint* OwningBlueprint, FName VariableName)
	{
		if (!IsDelegateCategory(PinType.PinCategory))
		{
			return nullptr;
		}

		UClass* GeneratedClass = IsValid(OwningBlueprint) ? OwningBlueprint->GeneratedClass.Get() : nullptr;

		// Resolve the member reference in the owning generated class when available.
		UFunction* SignatureFn = FMemberReference::ResolveSimpleMemberReference<UFunction>(
			PinType.PinSubCategoryMemberReference, GeneratedClass);
		if (IsValid(SignatureFn))
		{
			return SignatureFn;
		}

		// Blueprint dispatcher references can be empty or self-scoped; try the generated signature name.
		if (!IsValid(OwningBlueprint))
		{
			return nullptr;
		}

		const FName MemberSigName = PinType.PinSubCategoryMemberReference.MemberName;
		UClass* const CandidateClasses[] = { GeneratedClass, OwningBlueprint->SkeletonGeneratedClass.Get() };
		for (UClass* Candidate : CandidateClasses)
		{
			if (!IsValid(Candidate))
			{
				continue;
			}
			if (!VariableName.IsNone())
			{
				SignatureFn = Candidate->FindFunctionByName(
					FName(*(VariableName.ToString() + TEXT("__DelegateSignature"))));
				if (IsValid(SignatureFn))
				{
					return SignatureFn;
				}
			}
			if (!MemberSigName.IsNone())
			{
				SignatureFn = Candidate->FindFunctionByName(MemberSigName);
				if (IsValid(SignatureFn))
				{
					return SignatureFn;
				}
				SignatureFn = Candidate->FindFunctionByName(
					FName(*(MemberSigName.ToString() + TEXT("__DelegateSignature"))));
				if (IsValid(SignatureFn))
				{
					return SignatureFn;
				}
			}
		}
		return nullptr;
	}

	FString GetDelegateSignatureFunctionPath(const FEdGraphPinType& PinType,
		UBlueprint* OwningBlueprint, FName VariableName)
	{
		if (!IsDelegateCategory(PinType.PinCategory))
		{
			return FString();
		}
		if (const UFunction* Fn = ResolveDelegateSignatureFunction(PinType, OwningBlueprint, VariableName);
			IsValid(Fn))
		{
			return Fn->GetPathName();
		}

		// Fall back to the recorded parent path when no Blueprint scope is available.
		return GetDelegateSignaturePath(PinType);
	}

	void WriteVariableTypeJson(const FEdGraphPinType& PinType, const TSharedPtr<FJsonObject>& Out,
		UBlueprint* OwningBlueprint, FName VariableName)
	{
		if (!Out.IsValid())
		{
			return;
		}

		Out->SetStringField(TEXT("type"), FormatVariableTypeString(PinType));

		const bool bDelegate = IsDelegateCategory(PinType.PinCategory);
		if (!bDelegate)
		{
			Out->SetBoolField(TEXT("type_round_trips"), true);
			return;
		}

		const FString SignaturePath = GetDelegateSignatureFunctionPath(PinType, OwningBlueprint, VariableName);

		// Both delegate representations require a resolvable signature.
		Out->SetBoolField(TEXT("type_round_trips"), !SignaturePath.IsEmpty());

		// Always emit signature_function for delegates; empty means unresolved.
		Out->SetStringField(TEXT("signature_function"), SignaturePath);

		// Emit a replayable structured spec only when the signature resolves.
		if (!SignaturePath.IsEmpty())
		{
			TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
			Spec->SetStringField(TEXT("base"),
				PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate ? TEXT("mcdelegate") : TEXT("delegate"));
			Spec->SetStringField(TEXT("signature_function"), SignaturePath);
			Out->SetObjectField(TEXT("variable_type_spec"), Spec);
		}
		else
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("Variable '%s': could not resolve the signature UFunction for a %s variable; "
				     "signature_function is empty and type_round_trips is false."),
				*VariableName.ToString(),
				PinType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate ? TEXT("multicast delegate") : TEXT("delegate"));
		}
	}

	void WriteVariableCoreJson(const FBPVariableDescription& Var, const TSharedPtr<FJsonObject>& Out,
		UBlueprint* OwningBlueprint)
	{
		if (!Out.IsValid())
		{
			return;
		}

		Out->SetStringField(TEXT("variable_name"), Var.VarName.ToString());
		WriteVariableTypeJson(Var.VarType, Out, OwningBlueprint, Var.VarName);
		Out->SetStringField(TEXT("category"), Var.Category.ToString());
		Out->SetStringField(TEXT("display_name"), ResolveVariableDisplayName(Var));

		// Serialize effective flags, including the result of clear_flags.
		TArray<TSharedPtr<FJsonValue>> FlagValues;
		for (const FString& Flag : FormatPropertyFlags(Var.PropertyFlags))
		{
			FlagValues.Add(MakeShared<FJsonValueString>(Flag));
		}
		Out->SetArrayField(TEXT("flags"), FlagValues);

		// Recompose the replication spelling accepted by the setter.
		const bool bRepNotify = (Var.PropertyFlags & CPF_RepNotify) != 0;
		const bool bNet = (Var.PropertyFlags & CPF_Net) != 0;
		Out->SetStringField(TEXT("replication"),
			bRepNotify ? TEXT("RepNotify") : (bNet ? TEXT("Replicated") : TEXT("None")));
		Out->SetStringField(TEXT("rep_notify_func"),
			Var.RepNotifyFunc.IsNone() ? FString() : Var.RepNotifyFunc.ToString());

		Out->SetStringField(TEXT("replication_condition"),
			IsValid(StaticEnum<ELifetimeCondition>())
				? StaticEnum<ELifetimeCondition>()->GetNameStringByValue(static_cast<int64>(Var.ReplicationCondition))
				: FString());

		// Tooltip is also a first-class setter field.
		TSharedPtr<FJsonObject> Meta = MakeShared<FJsonObject>();
		FString Tooltip;
		for (const FBPVariableMetaDataEntry& Entry : Var.MetaDataArray)
		{
			Meta->SetStringField(Entry.DataKey.ToString(), Entry.DataValue);
			if (Entry.DataKey == FName(TEXT("tooltip")))
			{
				Tooltip = Entry.DataValue;
			}
		}
		Out->SetStringField(TEXT("tooltip"), Tooltip);
		Out->SetObjectField(TEXT("metadata"), Meta);
	}

	bool IsFunctionParameterSource(const UEdGraphPin* SourcePin)
	{
		if (!SourcePin || SourcePin->Direction != EGPD_Output)
		{
			return false;
		}
		if (SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			return false;
		}
		const UEdGraphNode* Owner = SourcePin->GetOwningNode();
		return IsValid(Owner) && Owner->IsA<UK2Node_FunctionEntry>();
	}

	const TCHAR* ToString(FCustomEventNameConflict::EKind Kind)
	{
		switch (Kind)
		{
		case FCustomEventNameConflict::EKind::ExistingOverrideEvent: return TEXT("existing_override_event");
		case FCustomEventNameConflict::EKind::ExistingCustomEvent:   return TEXT("existing_custom_event");
		case FCustomEventNameConflict::EKind::ParentBlueprintEvent:  return TEXT("parent_blueprint_event");
		case FCustomEventNameConflict::EKind::ParentFunction:        return TEXT("parent_function");
		case FCustomEventNameConflict::EKind::InterfaceFunction:     return TEXT("interface_function");
		case FCustomEventNameConflict::EKind::LocalFunctionGraph:    return TEXT("local_function_graph");
		default:                                                    return TEXT("none");
		}
	}

	FCustomEventNameConflict FindCustomEventNameConflict(
		UBlueprint* Blueprint,
		const FString& EventName,
		const UEdGraphNode* IgnoreNode)
	{
		FCustomEventNameConflict Conflict;
		if (!IsValid(Blueprint) || EventName.IsEmpty())
		{
			return Conflict;
		}

		// Check existing events across all graphs before creating a custom event.
		TArray<UEdGraph*> AllGraphs;
		Blueprint->GetAllGraphs(AllGraphs);
		for (const UEdGraph* Graph : AllGraphs)
		{
			if (!IsValid(Graph))
			{
				continue;
			}
			for (const UEdGraphNode* Node : Graph->Nodes)
			{
				const UK2Node_Event* Event = Cast<UK2Node_Event>(Node);
				if (!IsValid(Event) || Node == IgnoreNode)
				{
					continue;
				}
				if (!Event->GetFunctionName().ToString().Equals(EventName, ESearchCase::IgnoreCase))
				{
					continue;
				}

				Conflict.ResolvedFunctionName = Event->GetFunctionName();
				Conflict.ExistingNodeGuid = Event->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
				Conflict.ExistingGraphName = Graph->GetName();
				if (Event->bOverrideFunction)
				{
					Conflict.Kind = FCustomEventNameConflict::EKind::ExistingOverrideEvent;
					Conflict.OwnerName = IsValid(Event->EventReference.GetMemberParentClass())
						? Event->EventReference.GetMemberParentClass()->GetName()
						: FString();
					Conflict.Explanation = FString::Printf(
						TEXT("'%s' is already implemented in this Blueprint as the override event '%s' in graph '%s' (node %s)."),
						*EventName, *Conflict.ResolvedFunctionName.ToString(),
						*Conflict.ExistingGraphName, *Conflict.ExistingNodeGuid);
					Conflict.Remedy = FString::Printf(
						TEXT("Wire the new logic to that node -- reference it by GUID %s. A Custom Event of the same name is a second entry point that nothing calls, and the Blueprint will not compile."),
						*Conflict.ExistingNodeGuid);
				}
				else
				{
					Conflict.Kind = FCustomEventNameConflict::EKind::ExistingCustomEvent;
					Conflict.Explanation = FString::Printf(
						TEXT("A Custom Event named '%s' already exists in graph '%s' (node %s)."),
						*EventName, *Conflict.ExistingGraphName, *Conflict.ExistingNodeGuid);
					Conflict.Remedy = FString::Printf(
						TEXT("Reference the existing node by GUID %s instead of creating a second one; two Custom Events with one name do not compile."),
						*Conflict.ExistingNodeGuid);
				}
				return Conflict;
			}
		}

		// Parent Blueprint events require overrides, not custom events.
		if (UClass* ParentClass = Blueprint->ParentClass; IsValid(ParentClass))
		{
			ClaireonNameResolver::FNameResolveResult Result;
			if (UFunction* Found = ClaireonNameResolver::ResolveFunctionName(ParentClass, EventName, Result); IsValid(Found))
			{
				Conflict.ResolvedFunctionName = Found->GetFName();
				Conflict.OwnerName = IsValid(Found->GetOwnerClass()) ? Found->GetOwnerClass()->GetName() : ParentClass->GetName();
				Conflict.ResolutionNote = Result.ResolutionNote;
				Conflict.bNativeEvent = Found->HasAnyFunctionFlags(FUNC_Native);

				if (Found->HasAnyFunctionFlags(FUNC_BlueprintEvent))
				{
					Conflict.Kind = FCustomEventNameConflict::EKind::ParentBlueprintEvent;
					Conflict.Explanation = FString::Printf(
						TEXT("'%s' is a %s on %s, not a free name."),
						*EventName,
						Conflict.bNativeEvent ? TEXT("BlueprintNativeEvent") : TEXT("BlueprintImplementableEvent"),
						*Conflict.OwnerName);
					Conflict.Remedy = Conflict.bNativeEvent
						? FString::Printf(
							TEXT("Use the add_function_override operation with function_name='%s' so the graph implements the real event."),
							*Conflict.ResolvedFunctionName.ToString())
						: FString::Printf(
							TEXT("Create the override instead: node_type='EventOverride' with function_name='%s'. A Custom Event of this name does not override it, never runs, and does not compile."),
							*Conflict.ResolvedFunctionName.ToString());
				}
				else
				{
					Conflict.Kind = FCustomEventNameConflict::EKind::ParentFunction;
					Conflict.Explanation = FString::Printf(
						TEXT("'%s' is already a function on %s."),
						*EventName, *Conflict.OwnerName);
					Conflict.Remedy = FString::Printf(
						TEXT("Pick a different Custom Event name, or call '%s' with node_type='CallFunction'. A Custom Event cannot share a name with an inherited function."),
						*Conflict.ResolvedFunctionName.ToString());
				}
				return Conflict;
			}
		}

		// Check interface functions as well.
		for (const FBPInterfaceDescription& IfaceDesc : Blueprint->ImplementedInterfaces)
		{
			UClass* IfaceClass = IfaceDesc.Interface.Get();
			if (!IsValid(IfaceClass))
			{
				continue;
			}
			ClaireonNameResolver::FNameResolveResult Result;
			if (UFunction* Found = ClaireonNameResolver::ResolveFunctionName(IfaceClass, EventName, Result); IsValid(Found))
			{
				Conflict.Kind = FCustomEventNameConflict::EKind::InterfaceFunction;
				Conflict.ResolvedFunctionName = Found->GetFName();
				Conflict.OwnerName = IfaceClass->GetName();
				Conflict.ResolutionNote = Result.ResolutionNote;
				Conflict.Explanation = FString::Printf(
					TEXT("'%s' is a function on implemented interface %s."),
					*EventName, *Conflict.OwnerName);
				Conflict.Remedy = FString::Printf(
					TEXT("Implement the interface function rather than shadowing it: node_type='EventOverride' with function_name='%s' for an event-type interface function, or edit the interface function graph of that name. A Custom Event beside it is a different function that no interface call reaches."),
					*Conflict.ResolvedFunctionName.ToString());
				return Conflict;
			}
		}

		// Use function graphs; the skeleton class also contains custom events.
		for (const UEdGraph* FunctionGraph : Blueprint->FunctionGraphs)
		{
			if (IsValid(FunctionGraph) && FunctionGraph->GetName().Equals(EventName, ESearchCase::IgnoreCase))
			{
				Conflict.Kind = FCustomEventNameConflict::EKind::LocalFunctionGraph;
				Conflict.ResolvedFunctionName = FunctionGraph->GetFName();
				Conflict.OwnerName = FunctionGraph->GetName();
				Conflict.Explanation = FString::Printf(
					TEXT("This Blueprint already has a function graph named '%s'."), *EventName);
				Conflict.Remedy = FString::Printf(
					TEXT("Call it with node_type='CallFunction' and function_name='%s', or name the Custom Event something else."),
					*EventName);
				return Conflict;
			}
		}

		return Conflict;
	}

	bool IsDistantVariableGetSource(const UEdGraphPin* SourcePin, const UEdGraphPin* TargetPin, double MaxDistance)
	{
		if (!SourcePin || !TargetPin || SourcePin->Direction != EGPD_Output)
		{
			return false;
		}
		if (SourcePin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			return false;
		}

		const UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(SourcePin->GetOwningNode());
		const UEdGraphNode* Consumer = TargetPin->GetOwningNode();
		if (!IsValid(Get) || !IsValid(Consumer))
		{
			return false;
		}

		// Only pure, receiver-less reads can be copied beside a consumer.
		// Wired/default targets and validated gets carry semantics the copy would lose.
		for (const UEdGraphPin* Pin : Get->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				return false;
			}
			if (Pin->Direction == EGPD_Input
				&& (Pin->LinkedTo.Num() > 0 || IsValid(Pin->DefaultObject)))
			{
				return false;
			}
		}

		// Use anchor distance, as distant-get does.
		const double DX = static_cast<double>(Consumer->NodePosX - Get->NodePosX);
		const double DY = static_cast<double>(Consumer->NodePosY - Get->NodePosY);
		return FMath::Sqrt(DX * DX + DY * DY) > MaxDistance;
	}

	UEdGraphPin* EmitAdjacentVariableGet(UEdGraph* Graph, UEdGraphPin* SourcePin,
	                                     UEdGraphPin* TargetPin, int32 StackIndex)
	{
		UEdGraphNode* TargetNode = TargetPin ? TargetPin->GetOwningNode() : nullptr;
		UK2Node_VariableGet* Source = SourcePin ? Cast<UK2Node_VariableGet>(SourcePin->GetOwningNode()) : nullptr;
		if (!IsValid(Graph) || !IsValid(TargetNode) || !IsValid(Source))
		{
			return nullptr;
		}

		UK2Node_VariableGet* Get = NewObject<UK2Node_VariableGet>(Graph);
		Graph->AddNode(Get, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		// Preserve scope, owning class, self-context, and member GUID.
		Get->VariableReference = Source->VariableReference;
		Get->SelfContextInfo = Source->SelfContextInfo;
		Get->CreateNewGuid();
		Get->PostPlacedNewNode();
		Get->AllocateDefaultPins();

		Get->NodePosX = TargetNode->NodePosX - 240;
		Get->NodePosY = TargetNode->NodePosY + (StackIndex * 64);

		// Match the requested output by name for struct getters with multiple outputs.
		for (UEdGraphPin* Pin : Get->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output && Pin->PinName == SourcePin->PinName)
			{
				return Pin;
			}
		}
		for (UEdGraphPin* Pin : Get->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output
				&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}

		// Destroy an unresolved copy before returning failure so callers receive no orphan node.
		Get->DestroyNode();
		return nullptr;
	}

	UEdGraphPin* EmitLocalParameterGet(UEdGraph* Graph, UEdGraphPin* SourcePin,
	                                   UEdGraphPin* TargetPin, int32 StackIndex)
	{
		UEdGraphNode* TargetNode = TargetPin ? TargetPin->GetOwningNode() : nullptr;
		if (!IsValid(Graph) || !IsValid(TargetNode) || !SourcePin)
		{
			return nullptr;
		}

		UK2Node_VariableGet* Get = NewObject<UK2Node_VariableGet>(Graph);
		Graph->AddNode(Get, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Get->VariableReference.SetLocalMember(SourcePin->PinName, Graph->GetName(), FGuid());
		Get->CreateNewGuid();
		Get->PostPlacedNewNode();
		Get->AllocateDefaultPins();

		// Adjacent to the consumer, staggered so several gets do not stack.
		Get->NodePosX = TargetNode->NodePosX - 240;
		Get->NodePosY = TargetNode->NodePosY + (StackIndex * 64);

		for (UEdGraphPin* Pin : Get->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output
				&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}

		// An unresolved local reference has no value pin; remove the orphan before returning failure.
		Get->DestroyNode();
		return nullptr;
	}

	uint64 ParsePropertyFlags(const TArray<FString>& FlagStrings)
	{
		uint64 Flags = CPF_None;

		for (const FString& Flag : FlagStrings)
		{
			if (Flag == TEXT("BlueprintReadOnly"))
			{
				Flags |= CPF_BlueprintVisible | CPF_BlueprintReadOnly;
			}
			else if (Flag == TEXT("BlueprintReadWrite"))
			{
				Flags |= CPF_BlueprintVisible;
			}
			else if (Flag == TEXT("EditAnywhere"))
			{
				Flags |= CPF_Edit;
			}
			else if (Flag == TEXT("EditDefaultsOnly"))
			{
				Flags |= CPF_Edit | CPF_DisableEditOnInstance;
			}
			else if (Flag == TEXT("EditInstanceOnly"))
			{
				Flags |= CPF_Edit;
				Flags &= ~CPF_DisableEditOnInstance;
			}
			else if (Flag == TEXT("VisibleAnywhere"))
			{
				Flags |= CPF_Edit | CPF_EditConst;
			}
			else if (Flag == TEXT("Transient"))
			{
				Flags |= CPF_Transient;
			}
			else if (Flag == TEXT("Config"))
			{
				Flags |= CPF_Config;
			}
			else if (Flag == TEXT("SaveGame"))
			{
				Flags |= CPF_SaveGame;
			}
			else if (Flag == TEXT("Interp"))
			{
				Flags |= CPF_Interp;
			}
			else if (Flag == TEXT("ExposeOnSpawn"))
			{
				Flags |= CPF_ExposeOnSpawn;
			}
			else if (Flag == TEXT("Net") || Flag == TEXT("Replicated"))
			{
				Flags |= CPF_Net;
			}
			else if (Flag == TEXT("RepNotify"))
			{
				Flags |= CPF_Net | CPF_RepNotify;
			}
			else if (Flag == TEXT("AdvancedDisplay"))
			{
				Flags |= CPF_AdvancedDisplay;
			}
			else if (Flag == TEXT("AssetRegistrySearchable"))
			{
				Flags |= CPF_AssetRegistrySearchable;
			}
			else if (Flag == TEXT("SimpleDisplay"))
			{
				Flags |= CPF_SimpleDisplay;
			}
			else if (Flag == TEXT("DisableEditOnTemplate"))
			{
				Flags |= CPF_DisableEditOnTemplate;
			}
		}

		return Flags;
	}

	FString ResolveVariableDisplayName(const FBPVariableDescription& Var)
	{
		// Read display_name from the metadata key used by the setter.
		for (const FBPVariableMetaDataEntry& Entry : Var.MetaDataArray)
		{
			if (Entry.DataKey == FBlueprintMetadata::MD_DisplayName && !Entry.DataValue.IsEmpty())
			{
				return Entry.DataValue;
			}
		}
		return Var.FriendlyName;
	}
	TArray<FString> FormatPropertyFlags(uint64 PropertyFlags)
	{
		TArray<FString> Flags;

		// Blueprint visibility (compound flags)
		if (PropertyFlags & CPF_BlueprintVisible)
		{
			if (PropertyFlags & CPF_BlueprintReadOnly)
			{
				Flags.Add(TEXT("BlueprintReadOnly"));
			}
			else
			{
				Flags.Add(TEXT("BlueprintReadWrite"));
			}
		}

		// Replication (compound flags)
		if (PropertyFlags & CPF_Net)
		{
			if (PropertyFlags & CPF_RepNotify)
			{
				Flags.Add(TEXT("RepNotify"));
			}
			else
			{
				Flags.Add(TEXT("Net"));
			}
		}

		// Edit specifiers (compound flags)
		if (PropertyFlags & CPF_Edit)
		{
			if (PropertyFlags & CPF_DisableEditOnInstance)
			{
				Flags.Add(TEXT("EditDefaultsOnly"));
			}
			else if (PropertyFlags & CPF_EditConst)
			{
				Flags.Add(TEXT("VisibleAnywhere"));
			}
			else
			{
				Flags.Add(TEXT("EditAnywhere"));
			}
		}

		// Simple flags
		if (PropertyFlags & CPF_Transient)
		{
			Flags.Add(TEXT("Transient"));
		}
		if (PropertyFlags & CPF_Config)
		{
			Flags.Add(TEXT("Config"));
		}
		if (PropertyFlags & CPF_SaveGame)
		{
			Flags.Add(TEXT("SaveGame"));
		}
		if (PropertyFlags & CPF_Interp)
		{
			Flags.Add(TEXT("Interp"));
		}
		if (PropertyFlags & CPF_ExposeOnSpawn)
		{
			Flags.Add(TEXT("ExposeOnSpawn"));
		}
		if (PropertyFlags & CPF_AdvancedDisplay)
		{
			Flags.Add(TEXT("AdvancedDisplay"));
		}
		if (PropertyFlags & CPF_AssetRegistrySearchable)
		{
			Flags.Add(TEXT("AssetRegistrySearchable"));
		}
		if (PropertyFlags & CPF_SimpleDisplay)
		{
			Flags.Add(TEXT("SimpleDisplay"));
		}
		if (PropertyFlags & CPF_DisableEditOnTemplate)
		{
			Flags.Add(TEXT("DisableEditOnTemplate"));
		}

		return Flags;
	}

	void CreateBlueprint(const FString& AssetPath, UClass* ParentClass, FCreateBlueprintResult& OutResult,
	                     EBlueprintType BlueprintType)
	{
		if (!IsValid(ParentClass))
		{
			OutResult.Error = TEXT("ParentClass is null");
			return;
		}

		FString PackageName = AssetPath;
		FString AssetName;
		if (AssetPath.Contains(TEXT(".")))
		{
			FString TmpAssetPath = AssetPath;
			TmpAssetPath.Split(TEXT("."), &PackageName, &AssetName);
		}
		else
		{
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

		FString PackageFileName = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		if (FPaths::FileExists(PackageFileName))
		{
			UE_LOG(LogClaireon, Warning, TEXT("[CreateBlueprint] Deleting existing file %s"), *PackageFileName);
			IFileManager::Get().Delete(*PackageFileName, false, true);
		}

		UPackage* Package = CreatePackage(*PackageName);
		if (!IsValid(Package))
		{
			OutResult.Error = FString::Printf(TEXT("Failed to create package: %s"), *PackageName);
			return;
		}

		// Deleting the .uasset on disk above does not evict a same-named object still loaded in
		// memory; UE 5.8 CreateBlueprint asserts the name is free, so clear it first.
		ClaireonAssetUtils::EvictInMemoryObject(Package, AssetName);

		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			ParentClass, Package, FName(*AssetName),
			BlueprintType,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);

		if (!IsValid(BP))
		{
			OutResult.Error = FString::Printf(TEXT("Failed to create Blueprint at %s"), *AssetPath);
			return;
		}

		Package->SetIsExternallyReferenceable(true);
		Package->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(BP);

		// Inner-name/package-short-name invariant. Single guard point shared by
		// ClaireonBlueprintGraphTool_Create and the apply_spec Blueprint
		// applicator's OpenOrCreateAsset path. On mismatch, mark the partially-
		// created Blueprint garbage so it does not linger past GC.
		{
			FString AssertError;
			if (!ClaireonAssetUtils::AssertInnerNameMatchesPackage(BP, AssertError))
			{
				BP->ClearFlags(RF_Public | RF_Standalone);
				BP->MarkAsGarbage();
				OutResult.Error = AssertError;
				OutResult.Blueprint = nullptr;
				OutResult.EventGraph = nullptr;
				OutResult.Package = nullptr;
				return;
			}
		}

		UEdGraph* EventGraph = nullptr;
		if (BP->UbergraphPages.Num() > 0)
		{
			EventGraph = BP->UbergraphPages[0];
		}

		OutResult.Blueprint = BP;
		OutResult.EventGraph = EventGraph;
		OutResult.Package = Package;
	}

	void ApplyVariableProperties(UBlueprint* Blueprint, FName VarName, const TSharedPtr<FJsonObject>& Params, FApplyVariableResult* OutResult)
	{
		if (!IsValid(Blueprint) || !Params.IsValid())
		{
			return;
		}

		// Find the variable description
		FBPVariableDescription* VarDesc = nullptr;
		for (FBPVariableDescription& Var : Blueprint->NewVariables)
		{
			if (Var.VarName == VarName)
			{
				VarDesc = &Var;
				break;
			}
		}
		if (!VarDesc)
		{
			return;
		}

		// Track whether the replication field was explicitly provided
		bool bReplicationFieldProvided = false;

		// 1. Apply category
		FString Category;
		if (Params->TryGetStringField(TEXT("category"), Category))
		{
			FBlueprintEditorUtils::SetBlueprintVariableCategory(Blueprint, VarName, nullptr, FText::FromString(Category));
		}

		// 2. Apply tooltip
		FString Tooltip;
		if (Params->TryGetStringField(TEXT("tooltip"), Tooltip))
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_Tooltip, Tooltip);
		}

		// 3. Apply display_name
		FString DisplayName;
		if (Params->TryGetStringField(TEXT("display_name"), DisplayName))
		{
			FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FBlueprintMetadata::MD_DisplayName, DisplayName);
		}

		// 4. Apply replication
		FString Replication;
		if (Params->TryGetStringField(TEXT("replication"), Replication))
		{
			bReplicationFieldProvided = true;

			if (Replication.Equals(TEXT("None"), ESearchCase::IgnoreCase))
			{
				VarDesc->PropertyFlags &= ~(CPF_Net | CPF_RepNotify);
				VarDesc->RepNotifyFunc = NAME_None;
			}
			else if (Replication.Equals(TEXT("Replicated"), ESearchCase::IgnoreCase))
			{
				VarDesc->PropertyFlags |= CPF_Net;
				VarDesc->PropertyFlags &= ~CPF_RepNotify;
			}
			else if (Replication.Equals(TEXT("RepNotify"), ESearchCase::IgnoreCase)
			         || Replication.Equals(TEXT("rep_notify"), ESearchCase::IgnoreCase))
			{
				VarDesc->PropertyFlags |= CPF_Net | CPF_RepNotify;

				// Resolve the handler function name: caller-supplied or default OnRep_<VarName>.
				FString RepNotifyFuncStr;
				FName HandlerName;
				if (Params->TryGetStringField(TEXT("rep_notify_func"), RepNotifyFuncStr) && !RepNotifyFuncStr.IsEmpty())
				{
					HandlerName = FName(*RepNotifyFuncStr);
				}
				else
				{
					// Default UE5 convention: OnRep_VarName
					HandlerName = FName(*FString::Printf(TEXT("OnRep_%s"), *VarName.ToString()));
				}
				VarDesc->RepNotifyFunc = HandlerName;

				// Set ReplicationCondition if provided
				FString ReplicationCondition;
				if (Params->TryGetStringField(TEXT("replication_condition"), ReplicationCondition))
				{
					const UEnum* CondEnum = StaticEnum<ELifetimeCondition>();
					if (IsValid(CondEnum))
					{
						int64 CondValue = CondEnum->GetValueByNameString(ReplicationCondition);
						if (CondValue != INDEX_NONE)
						{
							VarDesc->ReplicationCondition = static_cast<ELifetimeCondition>(CondValue);
						}
					}
				}

				// Auto-create the handler function graph.
				// Follows editor precedent in FBlueprintVarActionDetails::ReplicationChanged
				// (Engine/Source/Editor/Kismet/Private/BlueprintDetailsCustomization.cpp ~lines 2773-2787).
				const FString HandlerNameStr = HandlerName.ToString();
				UEdGraph* FoundGraph = FindObject<UEdGraph>(Blueprint, *HandlerNameStr);

				// If the caller supplied a rep_notify_func that already resolves to a compiled
				// UFunction on the skeleton class, skip graph creation entirely -- they wired it
				// intentionally (proposal Risks #2).
				bool bUserFunctionExists = false;
				if (Blueprint->SkeletonGeneratedClass != nullptr)
				{
					bUserFunctionExists = (Blueprint->SkeletonGeneratedClass->FindFunctionByName(HandlerName) != nullptr);
				}

				if (!bUserFunctionExists && FoundGraph == nullptr)
				{
					UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
						Blueprint,
						HandlerName,
						UEdGraph::StaticClass(),
						UEdGraphSchema_K2::StaticClass());
					FBlueprintEditorUtils::AddFunctionGraph<UClass>(
						Blueprint,
						NewGraph,
						/*bIsUserCreated=*/false,
						/*SignatureFromClass=*/nullptr);
					FoundGraph = NewGraph;

					if (OutResult != nullptr)
					{
						OutResult->bRepNotifyGraphCreated = true;
					}
				}

				if (OutResult != nullptr && FoundGraph != nullptr)
				{
					OutResult->RepNotifyHandlerGraph = HandlerName;
				}
			}
		}

		// 5. Apply metadata entries
		const TSharedPtr<FJsonObject>* MetadataObj = nullptr;
		if (Params->TryGetObjectField(TEXT("metadata"), MetadataObj) && MetadataObj && (*MetadataObj).IsValid())
		{
			for (const auto& Pair : (*MetadataObj)->Values)
			{
				FString Value;
				if (Pair.Value.IsValid() && Pair.Value->TryGetString(Value))
				{
					FBlueprintEditorUtils::SetBlueprintVariableMetaData(Blueprint, VarName, nullptr, FName(*Pair.Key), Value);
				}
			}
		}

		// 6. Apply flags[]
		const TArray<TSharedPtr<FJsonValue>>* FlagsArray = nullptr;
		if (Params->TryGetArrayField(TEXT("flags"), FlagsArray))
		{
			TArray<FString> RemainingFlags;

			for (const TSharedPtr<FJsonValue>& FlagValue : *FlagsArray)
			{
				FString Flag = FlagValue->AsString();

				if (Flag == TEXT("Interp"))
				{
					FBlueprintEditorUtils::SetInterpFlag(Blueprint, VarName, true);
				}
				else if (Flag == TEXT("BlueprintReadOnly"))
				{
					FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, false);
					FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, VarName, true);
					VarDesc->PropertyFlags |= CPF_BlueprintVisible;
				}
				else if (Flag == TEXT("BlueprintReadWrite"))
				{
					FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, true);
					FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, VarName, false);
					VarDesc->PropertyFlags |= CPF_BlueprintVisible;
				}
				else if (Flag == TEXT("EditInstanceOnly"))
				{
					// EditInstanceOnly must clear CPF_DisableEditOnInstance; OR-ing parsed flags cannot clear it.
					FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, false);
					VarDesc->PropertyFlags |= CPF_Edit;
				}
				else if (bReplicationFieldProvided && (Flag == TEXT("Net") || Flag == TEXT("Replicated") || Flag == TEXT("RepNotify")))
				{
					// Skip replication flags if replication field was explicitly provided
					continue;
				}
				else
				{
					RemainingFlags.Add(Flag);
				}
			}

			if (RemainingFlags.Num() > 0)
			{
				VarDesc->PropertyFlags |= ParsePropertyFlags(RemainingFlags);
			}
		}

		// 7. Apply clear_flags[]
		const TArray<TSharedPtr<FJsonValue>>* ClearFlagsArray = nullptr;
		if (Params->TryGetArrayField(TEXT("clear_flags"), ClearFlagsArray))
		{
			TArray<FString> RemainingClearFlags;

			for (const TSharedPtr<FJsonValue>& FlagValue : *ClearFlagsArray)
			{
				FString Flag = FlagValue->AsString();

				if (Flag == TEXT("Interp"))
				{
					FBlueprintEditorUtils::SetInterpFlag(Blueprint, VarName, false);
				}
				else if (Flag == TEXT("BlueprintReadOnly"))
				{
					FBlueprintEditorUtils::SetBlueprintPropertyReadOnlyFlag(Blueprint, VarName, false);
				}
				else if (Flag == TEXT("BlueprintReadWrite"))
				{
					FBlueprintEditorUtils::SetBlueprintOnlyEditableFlag(Blueprint, VarName, false);
				}
				else
				{
					RemainingClearFlags.Add(Flag);
				}
			}

			if (RemainingClearFlags.Num() > 0)
			{
				VarDesc->PropertyFlags &= ~ParsePropertyFlags(RemainingClearFlags);
			}
		}
	}

	bool CreateVariableFromSpec(UBlueprint* Blueprint,
	                            const TSharedPtr<FJsonObject>& Params,
	                            FApplyVariableResult* OutResult,
	                            FString& OutError)
	{
		if (!IsValid(Blueprint))
		{
			OutError = TEXT("Blueprint is null");
			return false;
		}
		if (!Params.IsValid())
		{
			OutError = TEXT("Params is null");
			return false;
		}

		FString VarName;
		if (!Params->TryGetStringField(TEXT("variable_name"), VarName) &&
			!Params->TryGetStringField(TEXT("name"), VarName))
		{
			OutError = TEXT("Missing required field: variable_name (or 'name')");
			return false;
		}

		FString VarType;
		const TSharedPtr<FJsonObject>* TypeSpecObj = nullptr;
		const bool bHasTypeSpec = Params->TryGetObjectField(TEXT("variable_type_spec"), TypeSpecObj)
			&& TypeSpecObj && (*TypeSpecObj).IsValid();
		const bool bHasTypeString = Params->TryGetStringField(TEXT("variable_type"), VarType)
			|| Params->TryGetStringField(TEXT("type"), VarType);
		if (!bHasTypeSpec && !bHasTypeString)
		{
			OutError = TEXT("Missing required field: variable_type (or variable_type_spec)");
			return false;
		}

		FParseVariableTypeResult ParseResult = bHasTypeSpec
			? ParseVariableTypeSpec(*TypeSpecObj)
			: ParseVariableTypeChecked(VarType);
		if (!ParseResult.bSucceeded)
		{
			OutError = FString::Printf(TEXT("Failed to parse variable type: %s"), *ParseResult.Error);
			return false;
		}

		// Standalone container_type param ('array' | 'set' | 'map'): schema-advertised
		// alternative to the Array<T>/Set<T>/Map<K,V> angle forms. Silently dropping it
		// created scalar variables from Set/Array requests (GA 'Hit Actors' repro).
		FString ContainerTypeStr;
		if (Params->TryGetStringField(TEXT("container_type"), ContainerTypeStr) && !ContainerTypeStr.IsEmpty()
			&& !ContainerTypeStr.Equals(TEXT("none"), ESearchCase::IgnoreCase))
		{
			if (ParseResult.PinType.ContainerType != EPinContainerType::None)
			{
				// Angle form already set a container; the explicit param must agree.
				const TCHAR* Existing =
					ParseResult.PinType.ContainerType == EPinContainerType::Array ? TEXT("array") :
					ParseResult.PinType.ContainerType == EPinContainerType::Set ? TEXT("set") : TEXT("map");
				if (!ContainerTypeStr.Equals(Existing, ESearchCase::IgnoreCase))
				{
					OutError = FString::Printf(
						TEXT("container_type '%s' conflicts with the '%s' container already expressed by the variable type"),
						*ContainerTypeStr, Existing);
					return false;
				}
			}
			else if (ContainerTypeStr.Equals(TEXT("array"), ESearchCase::IgnoreCase))
			{
				ParseResult.PinType.ContainerType = EPinContainerType::Array;
			}
			else if (ContainerTypeStr.Equals(TEXT("set"), ESearchCase::IgnoreCase))
			{
				ParseResult.PinType.ContainerType = EPinContainerType::Set;
			}
			else if (ContainerTypeStr.Equals(TEXT("map"), ESearchCase::IgnoreCase))
			{
				// A map needs a value terminal; a bare container_type='map' cannot supply
				// one, so require the Map<K,V> form instead of guessing.
				OutError = TEXT("container_type 'map' requires the Map<K,V> variable_type form (the value type cannot be inferred)");
				return false;
			}
			else
			{
				OutError = FString::Printf(TEXT("Unknown container_type '%s' (expected 'none', 'array', 'set', or 'map')"), *ContainerTypeStr);
				return false;
			}
		}

		for (const FBPVariableDescription& Existing : Blueprint->NewVariables)
		{
			if (Existing.VarName == FName(*VarName))
			{
				OutError = FString::Printf(TEXT("Variable '%s' already exists"), *VarName);
				return false;
			}
		}

		FBPVariableDescription NewVar;
		NewVar.VarName = FName(*VarName);
		NewVar.VarGuid = FGuid::NewGuid();
		NewVar.VarType = ParseResult.PinType;
		NewVar.FriendlyName = VarName;
		NewVar.Category = FText::FromString(TEXT("Default"));
		NewVar.PropertyFlags = CPF_Edit | CPF_BlueprintVisible;

		// Event dispatchers need the dispatcher flags (CallDelegate/AddDelegate/
		// ClearDelegate nodes reject non-assignable delegates at compile) and a
		// delegate signature graph -- the same setup the editor's 'Add Event
		// Dispatcher' action performs.
		const bool bIsMulticastDelegate = (NewVar.VarType.PinCategory == UEdGraphSchema_K2::PC_MCDelegate);
		if (bIsMulticastDelegate)
		{
			NewVar.PropertyFlags |= CPF_BlueprintAssignable | CPF_BlueprintCallable;
		}

		const TArray<TSharedPtr<FJsonValue>>* FlagsArray = nullptr;
		if (Params->TryGetArrayField(TEXT("flags"), FlagsArray) && FlagsArray)
		{
			TArray<FString> FlagNames;
			for (const TSharedPtr<FJsonValue>& FlagVal : *FlagsArray)
			{
				FlagNames.Add(FlagVal->AsString());
			}
			NewVar.PropertyFlags |= ParsePropertyFlags(FlagNames);
		}

		FString DefaultValue;
		if (Params->TryGetStringField(TEXT("default_value"), DefaultValue))
		{
			NewVar.DefaultValue = DefaultValue;
		}

		Blueprint->NewVariables.Add(NewVar);

		// Dispatcher signature graph (SMyBlueprint::OnAddNewDelegate idiom). When the
		// caller supplied a signature_function, mirror its parameters onto the entry
		// node so the compiled '<Var>__DelegateSignature' matches the requested shape.
		if (bIsMulticastDelegate && !FindObject<UEdGraph>(Blueprint, *VarName))
		{
			UEdGraph* SignatureGraph = FBlueprintEditorUtils::CreateNewGraph(
				Blueprint, FName(*VarName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
			if (IsValid(SignatureGraph))
			{
				SignatureGraph->bEditable = false;
				const UEdGraphSchema_K2* K2Schema = GetDefault<UEdGraphSchema_K2>();
				K2Schema->CreateDefaultNodesForGraph(*SignatureGraph);
				K2Schema->CreateFunctionGraphTerminators(*SignatureGraph, (UClass*)nullptr);
				K2Schema->AddExtraFunctionFlags(SignatureGraph, (FUNC_BlueprintCallable | FUNC_BlueprintEvent | FUNC_Public));
				K2Schema->MarkFunctionEntryAsEditable(SignatureGraph, true);
				Blueprint->DelegateSignatureGraphs.Add(SignatureGraph);

				// Copy parameters from the resolved signature function, if any.
				UFunction* SigFn = FMemberReference::ResolveSimpleMemberReference<UFunction>(
					NewVar.VarType.PinSubCategoryMemberReference, Blueprint->GeneratedClass);
				if (IsValid(SigFn))
				{
					UK2Node_FunctionEntry* SigEntry = nullptr;
					for (UEdGraphNode* Node : SignatureGraph->Nodes)
					{
						if (UK2Node_FunctionEntry* AsEntry = Cast<UK2Node_FunctionEntry>(Node); IsValid(AsEntry))
						{
							SigEntry = AsEntry;
							break;
						}
					}
					if (IsValid(SigEntry))
					{
						const UEdGraphSchema_K2* Schema = GetDefault<UEdGraphSchema_K2>();
						for (TFieldIterator<FProperty> ParamIt(SigFn); ParamIt && (ParamIt->PropertyFlags & CPF_Parm); ++ParamIt)
						{
							if (ParamIt->PropertyFlags & CPF_ReturnParm) continue;
							FEdGraphPinType ParamPinType;
							if (Schema->ConvertPropertyToPinType(*ParamIt, ParamPinType))
							{
								SigEntry->CreateUserDefinedPin(ParamIt->GetFName(), ParamPinType, EGPD_Output);
							}
						}
					}
				}
			}
		}

		ApplyVariableProperties(Blueprint, FName(*VarName), Params, OutResult);

		return true;
	}

	UEdGraphPin* GetFirstOutputPin(UEdGraphNode* Node)
	{
		if (!IsValid(Node))
		{
			return nullptr;
		}

		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output)
			{
				return Pin;
			}
		}

		return nullptr;
	}

	FString FormatAvailableNodes(UEdGraph* Graph, int32 MaxCount)
	{
		if (!IsValid(Graph))
			return TEXT("");

		FString Result;
		int32 TotalNodes = Graph->Nodes.Num();
		int32 Count = 0;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node))
				continue;
			if (Count >= MaxCount)
				break;

			FString Title = Node->GetNodeTitle(ENodeTitleType::ListView).ToString();
			FString ClassName = Node->GetClass()->GetName();
			Result += FString::Printf(TEXT("  %s - %s \"%s\"\n"),
				*Node->NodeGuid.ToString(), *ClassName, *Title);
			Count++;
		}

		if (TotalNodes > MaxCount)
		{
			Result += FString::Printf(TEXT("  ... and %d more\n"), TotalNodes - MaxCount);
		}

		return FString::Printf(TEXT("Available nodes (%d of %d):\n%s"),
			FMath::Min(Count, MaxCount), TotalNodes, *Result);
	}

	FString FormatAvailablePins(UEdGraphNode* Node)
	{
		if (!IsValid(Node))
			return TEXT("");

		FString Result;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
				continue;
			FString Direction = (Pin->Direction == EGPD_Input) ? TEXT("input") : TEXT("output");
			Result += FString::Printf(TEXT("  %s (%s)\n"), *Pin->GetName(), *Direction);
		}
		return Result;
	}

	TArray<UEdGraphNode*> FindNodesByClassAndTitle(UEdGraph* Graph, const FString& ClassName, const FString& Title)
	{
		TArray<UEdGraphNode*> Results;
		if (!IsValid(Graph))
			return Results;

		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node))
				continue;

			bool bClassMatch = ClassName.IsEmpty() || Node->GetClass()->GetName().Contains(ClassName);
			bool bTitleMatch = Title.IsEmpty() || Node->GetNodeTitle(ENodeTitleType::ListView).ToString().Contains(Title);

			if (bClassMatch && bTitleMatch)
			{
				Results.Add(Node);
			}
		}
		return Results;
	}

	bool IsBlueprintAssetClass(const FString& ClassName)
	{
		return ClassName == TEXT("Blueprint")
			|| ClassName == TEXT("AnimBlueprint")
			|| ClassName == TEXT("WidgetBlueprint");
	}

	/**
	 * Resolve an editor-only K2Node class by short name, without linking its module.
	 *
	 * UK2Node_LatentAbilityCall (GameplayAbilitiesEditor) and
	 * UK2Node_LatentGameplayTaskCall (GameplayTasksEditor) are needed only as UClass
	 * pointers; including their headers would add editor-module dependencies to
	 * Claireon.Build.cs for no other benefit. Returns null when the module is not
	 * loaded, so callers can fall back.
	 */
	static UClass* FindLatentTaskNodeClassByName(const TCHAR* ShortClassName)
	{
		if (UClass* Direct = FindFirstObject<UClass>(ShortClassName, EFindFirstObjectOptions::NativeFirst); IsValid(Direct))
		{
			return Direct;
		}
		ClaireonNameResolver::FNameResolveResult Result;
		return ClaireonNameResolver::ResolveClassName(ShortClassName, UEdGraphNode::StaticClass(), Result);
	}

	UClass* PickK2NodeClassForFunction(const UFunction* Function)
	{
		if (!IsValid(Function))
		{
			return UK2Node_CallFunction::StaticClass();
		}

		const bool bIsPure = Function->HasAllFunctionFlags(FUNC_BlueprintPure);
		const bool bHasArrayPointerParms = Function->HasMetaData(FBlueprintMetadata::MD_ArrayParam);
		const bool bIsCommutativeAssociativeBinaryOp = Function->HasMetaData(FBlueprintMetadata::MD_CommutativeAssociativeBinaryOperator);
		const bool bIsMaterialParamCollectionFunc = Function->HasMetaData(FBlueprintMetadata::MD_MaterialParameterCollectionFunction);
		const bool bIsDataTableFunc = Function->HasMetaData(FBlueprintMetadata::MD_DataTablePin);

		// AsyncAction detection: mirror UK2Node_AsyncAction::GetMenuActions filter.
		// Functions whose owning class carries HasDedicatedAsyncNode metadata fall
		// through to the plain UK2Node_CallFunction path (today's behavior).
		if (const UClass* OwnerClass = Function->GetOwnerClass(); IsValid(OwnerClass))
		{
			if (OwnerClass->IsChildOf(UBlueprintAsyncActionBase::StaticClass())
				&& !OwnerClass->HasMetaData(TEXT("HasDedicatedAsyncNode")))
			{
				if (const FObjectProperty* ReturnProp = CastField<FObjectProperty>(Function->GetReturnProperty()))
				{
					if (ReturnProp->PropertyClass
						&& ReturnProp->PropertyClass->IsChildOf(UBlueprintAsyncActionBase::StaticClass()))
					{
						return UK2Node_AsyncAction::StaticClass();
					}
				}
			}
		}

		// GameplayTask factories get their dedicated latent node instead of a plain
		// CallFunction, which is what the editor's own menu produces and what the
		// generated graph needs to compile. UAbilityTask is the narrower case:
		// UK2Node_LatentAbilityCall derives from UK2Node_LatentGameplayTaskCall.
		//
		// Both node classes live in editor-only modules (GameplayAbilitiesEditor /
		// GameplayTasksEditor) that Claireon deliberately does not depend on, so they
		// are resolved by name -- no #include, no new Build.cs edge. A name that does
		// not resolve simply falls through to the CallFunction default.
		if (const FObjectProperty* ReturnProp = CastField<FObjectProperty>(Function->GetReturnProperty()))
		{
			UClass* ReturnClass = ReturnProp->PropertyClass;
			if (IsValid(ReturnClass) && ReturnClass->IsChildOf(UGameplayTask::StaticClass()))
			{
				const TCHAR* DesiredNodeClassName = ReturnClass->IsChildOf(UAbilityTask::StaticClass())
					? TEXT("K2Node_LatentAbilityCall")
					: TEXT("K2Node_LatentGameplayTaskCall");
				if (UClass* LatentNodeClass = FindLatentTaskNodeClassByName(DesiredNodeClassName); IsValid(LatentNodeClass))
				{
					return LatentNodeClass;
				}
			}
		}

		if (bIsCommutativeAssociativeBinaryOp && bIsPure)
		{
			return UK2Node_CommutativeAssociativeBinaryOperator::StaticClass();
		}
		if (bIsMaterialParamCollectionFunc)
		{
			return UK2Node_CallMaterialParameterCollectionFunction::StaticClass();
		}
		if (bIsDataTableFunc)
		{
			return UK2Node_CallDataTableFunction::StaticClass();
		}
		if (bHasArrayPointerParms)
		{
			return UK2Node_CallArrayFunction::StaticClass();
		}
		return UK2Node_CallFunction::StaticClass();
	}

	FString GetNodeTypeAliasForClass(const UClass* NodeClass)
	{
		return ::ClaireonNodeTypeAlias::GetAliasForNodeClass(NodeClass);
	}
} // namespace ClaireonBlueprintHelpers

bool ClaireonBlueprintHelpers::ResolveComponentTemplate(UBlueprint* Blueprint, FName ComponentName, bool bForWrite, FResolvedComponentTemplate& OutResolved, FString& OutError)
{
	OutResolved = FResolvedComponentTemplate();
	if (!IsValid(Blueprint))
	{
		OutError = TEXT("Blueprint is invalid");
		return false;
	}

	// Own SCS first: the common case, and the only place a non-inherited component can be.
	if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
	{
		if (USCS_Node* Node = SCS->FindSCSNode(ComponentName))
		{
			OutResolved.Node = Node;
			OutResolved.Template = Node->ComponentTemplate;
			if (!IsValid(OutResolved.Template))
			{
				OutError = FString::Printf(TEXT("Component '%s' has no template object"), *ComponentName.ToString());
				return false;
			}
			return true;
		}
	}

	// Then every ancestor Blueprint's SCS. An inherited component is edited through this
	// Blueprint's InheritableComponentHandler override template (what the Details panel
	// creates on first edit); writing to the parent's template would change the parent.
	for (UClass* Class = Blueprint->ParentClass; Class; Class = Class->GetSuperClass())
	{
		UBlueprintGeneratedClass* ParentBPGC = Cast<UBlueprintGeneratedClass>(Class);
		if (!ParentBPGC || !ParentBPGC->SimpleConstructionScript)
		{
			continue;
		}
		USCS_Node* ParentNode = ParentBPGC->SimpleConstructionScript->FindSCSNode(ComponentName);
		if (!ParentNode)
		{
			continue;
		}

		OutResolved.Node = ParentNode;
		OutResolved.bInherited = true;

		const FComponentKey Key(ParentNode);
		if (UInheritableComponentHandler* Handler = Blueprint->GetInheritableComponentHandler(/*bCreateIfNecessary=*/bForWrite))
		{
			UActorComponent* Override = Handler->GetOverridenComponentTemplate(Key);
			if (!Override && bForWrite)
			{
				Override = Handler->CreateOverridenComponentTemplate(Key);
			}
			if (Override)
			{
				OutResolved.Template = Override;
				OutResolved.bOverridden = true;
				return true;
			}
		}

		if (bForWrite)
		{
			OutError = FString::Printf(TEXT("Component '%s' is inherited from %s and no override template could be created"),
				*ComponentName.ToString(), *ParentBPGC->GetName());
			return false;
		}
		OutResolved.Template = ParentNode->ComponentTemplate;
		if (!IsValid(OutResolved.Template))
		{
			OutError = FString::Printf(TEXT("Inherited component '%s' has no template object"), *ComponentName.ToString());
			return false;
		}
		return true;
	}

	OutError = FString::Printf(TEXT("Component not found: %s (searched this Blueprint and its Blueprint ancestors; native C++ components are not SCS components)"), *ComponentName.ToString());
	return false;
}
