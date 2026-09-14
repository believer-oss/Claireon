// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// PCG does not rebuild editor counterparts for runtime graph edits. Its reconstruction
// API is unexported, so reconciliation uses reflected editor-node classes and the
// PCGNode property, then virtual ReconstructNode calls to rebuild pins and links.
// All peers and their pins must exist before reconstruction.
// Synthesized nodes subscribe to runtime changes but lack native enabled-state visuals,
// error badges, and dynamic-pin add/remove controls.

#include "Tools/ClaireonPCGEditorSync.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ClaireonLog.h"
#include "Containers/Ticker.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Editor.h"
#include "Misc/EngineVersionComparison.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "PCGSettings.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/ObjectKey.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"

namespace ClaireonPCGEditorSync_Impl
{
	// UE 5.8 replaces the nested-object bool argument with EGetObjectsFlags.
#if UE_VERSION_OLDER_THAN(5, 8, 0)
	constexpr bool GNoNestedObjects = false;
#else
	constexpr EGetObjectsFlags GNoNestedObjects = EGetObjectsFlags::None;
#endif

	/** Graphs waiting for the next flush. Weak: an edited graph can be GC'd before the tick. */
	static TSet<TWeakObjectPtr<UPCGGraph>> PendingGraphs;

	/** Graphs already warned about, so a warning does not repeat on every edit of a burst. */
	static TSet<TWeakObjectPtr<UPCGGraph>> WarnedGraphs;

	/** Exactly one flush is ever scheduled at a time. */
	static FTSTicker::FDelegateHandle FlushHandle;
	static bool bFlushScheduled = false;

	static ClaireonPCGEditorSync::FReconstructStats SyncStats;

	// Subscribe synthesized nodes to native edits because their unexported Construct()
	// method cannot establish the usual OnNodeChangedDelegate binding.
	struct FSynthesizedNodeBridge
	{
		FDelegateHandle Handle;
		TWeakObjectPtr<UPCGNode> PCGNode;
		TWeakObjectPtr<UEdGraphNode> EdNode;
	};

	static TMap<FObjectKey, FSynthesizedNodeBridge> SynthesizedNodeBridges;

	static void UnbindSynthesizedNodeBridge(const FObjectKey& Key)
	{
		FSynthesizedNodeBridge Bridge;
		if (SynthesizedNodeBridges.RemoveAndCopyValue(Key, Bridge))
		{
			if (UPCGNode* Node = Bridge.PCGNode.Get())
			{
				Node->OnNodeChangedDelegate.Remove(Bridge.Handle);
			}
		}
	}

	static void UnbindBridgesForEdNode(const UEdGraphNode* EdNode)
	{
		TArray<FObjectKey> Keys;
		for (const TPair<FObjectKey, FSynthesizedNodeBridge>& Pair : SynthesizedNodeBridges)
		{
			if (Pair.Value.EdNode.Get() == EdNode)
			{
				Keys.Add(Pair.Key);
			}
		}
		for (const FObjectKey& Key : Keys)
		{
			UnbindSynthesizedNodeBridge(Key);
		}
	}

	static void BindSynthesizedNodeBridge(UPCGNode* PCGNode, UEdGraphNode* EdNode)
	{
		const FObjectKey Key(PCGNode);
		if (const FSynthesizedNodeBridge* Existing = SynthesizedNodeBridges.Find(Key))
		{
			if (Existing->EdNode.Get() == EdNode)
			{
				return;
			}
			UnbindSynthesizedNodeBridge(Key);
		}

		FSynthesizedNodeBridge Bridge;
		Bridge.PCGNode = PCGNode;
		Bridge.EdNode = EdNode;
		const TWeakObjectPtr<UEdGraphNode> EdWeak(EdNode);
		Bridge.Handle = PCGNode->OnNodeChangedDelegate.AddLambda(
			[EdWeak, Key](UPCGNode* /*ChangedNode*/, EPCGChangeType ChangeType)
			{
				if (!IsInGameThread())
				{
					return;
				}
				// The native handler's rebuild mask. Settings-only changes that affect
				// pins arrive with Structural set, so they are covered.
				const EPCGChangeType RebuildMask = EPCGChangeType::Structural
					| EPCGChangeType::Node | EPCGChangeType::Edge | EPCGChangeType::Cosmetic;
				if (!(ChangeType & RebuildMask))
				{
					return;
				}
				UEdGraphNode* Ed = EdWeak.Get();
				if (!IsValid(Ed))
				{
					UnbindSynthesizedNodeBridge(Key);
					return;
				}
				Ed->ReconstructNode();
				if (UEdGraph* Graph = Ed->GetGraph())
				{
					Graph->NotifyGraphChanged();
				}
			});
		SynthesizedNodeBridges.Add(Key, MoveTemp(Bridge));
	}

	/** Test seam: a class name FindPCGEditorClass reports as absent. NAME_None when unset. */
	static FName MissingEditorNodeClassForTests = NAME_None;

	/** Reflected editor-node class, or null if this engine does not have it under that name. */
	static UClass* FindPCGEditorClass(const TCHAR* ClassName)
	{
		if (!MissingEditorNodeClassForTests.IsNone() && MissingEditorNodeClassForTests == FName(ClassName))
		{
			return nullptr;
		}
		const FString Path = FString::Printf(TEXT("/Script/PCGEditor.%s"), ClassName);
		return FindObject<UClass>(nullptr, *Path);
	}

	/** The base class every PCG editor node derives from; the gate for this whole path. */
	static UClass* PCGEditorNodeBaseClass()
	{
		static UClass* Cached = nullptr;
		if (!IsValid(Cached))
		{
			Cached = FindPCGEditorClass(TEXT("PCGEditorGraphNodeBase"));
		}
		return Cached;
	}

	/** The UPROPERTY tying an editor node to its runtime PCG node. */
	static FObjectPropertyBase* FindPCGNodeProperty(const UClass* NodeClass)
	{
		if (!IsValid(NodeClass))
		{
			return nullptr;
		}
		return CastField<FObjectPropertyBase>(NodeClass->FindPropertyByName(TEXT("PCGNode")));
	}

	static UPCGNode* GetLinkedPCGNode(UEdGraphNode* EdNode)
	{
		if (!IsValid(EdNode))
		{
			return nullptr;
		}
		if (FObjectPropertyBase* Property = FindPCGNodeProperty(EdNode->GetClass()))
		{
			return Cast<UPCGNode>(Property->GetObjectPropertyValue(Property->ContainerPtrToValuePtr<void>(EdNode)));
		}
		return nullptr;
	}

	static bool SetLinkedPCGNode(UEdGraphNode* EdNode, UPCGNode* PCGNode)
	{
		if (!IsValid(EdNode) || !IsValid(PCGNode))
		{
			return false;
		}
		FObjectPropertyBase* Property = FindPCGNodeProperty(EdNode->GetClass());
		if (!Property)
		{
			return false;
		}
		Property->SetObjectPropertyValue(Property->ContainerPtrToValuePtr<void>(EdNode), PCGNode);
		return true;
	}

	/**
	 * Which editor node class represents this PCG node.
	 *
	 * Mirrors the mapping in UPCGEditorGraph::ReconstructGraph (PCGEditorGraph.cpp:88-135).
	 * Matched on settings class NAME so this file needs no PCG type it would have to link
	 * against, and an unknown settings class falls back to the general node.
	 */
	static const TCHAR* EditorNodeClassNameFor(const UPCGGraph* Graph, const UPCGNode* PCGNode)
	{
		if (IsValid(Graph) && PCGNode == Graph->GetInputNode())
		{
			return TEXT("PCGEditorGraphNodeInput");
		}
		if (IsValid(Graph) && PCGNode == Graph->GetOutputNode())
		{
			return TEXT("PCGEditorGraphNodeOutput");
		}

		const UPCGSettings* Settings = IsValid(PCGNode) ? PCGNode->GetSettings() : nullptr;
		const FString SettingsClass = IsValid(Settings) ? Settings->GetClass()->GetName() : FString();

		if (SettingsClass == TEXT("PCGNamedRerouteDeclarationSettings"))
		{
			return TEXT("PCGEditorGraphNodeNamedRerouteDeclaration");
		}
		if (SettingsClass == TEXT("PCGNamedRerouteUsageSettings"))
		{
			return TEXT("PCGEditorGraphNodeNamedRerouteUsage");
		}
		if (SettingsClass == TEXT("PCGRerouteSettings"))
		{
			return TEXT("PCGEditorGraphNodeReroute");
		}

		return TEXT("PCGEditorGraphNode");
	}

	/**
	 * The cached editor graph for InGraph, if one has ever been built.
	 *
	 * UPCGGraph::PCGEditorGraph is protected, so it cannot be read directly -- but the engine
	 * creates the editor graph outered to the PCG graph (PCGEditor.cpp:108) and
	 * UPCGEditorGraph derives from UEdGraph, which is public and exported.
	 */
	static UEdGraph* FindEditorGraph(UPCGGraph* InGraph)
	{
		if (!IsValid(InGraph))
		{
			return nullptr;
		}

		UEdGraph* Found = nullptr;
		ForEachObjectWithOuter(InGraph, [&Found](UObject* Object)
		{
			if (!IsValid(Found))
			{
				if (UEdGraph* AsEdGraph = Cast<UEdGraph>(Object); IsValid(AsEdGraph))
				{
					Found = AsEdGraph;
				}
			}
		}, GNoNestedObjects);

		return Found;
	}

	/** True when an asset editor window is currently open for InGraph. Does not focus it. */
	static bool IsAssetEditorOpenFor(UPCGGraph* InGraph)
	{
		if (!IsValid(InGraph) || !IsValid(GEditor))
		{
			return false;
		}

		UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (!IsValid(Subsystem))
		{
			return false;
		}

		return Subsystem->FindEditorForAsset(InGraph, /*bFocusIfOpen=*/false) != nullptr;
	}

	static void FlushPending();

	static void EnsureFlushScheduled()
	{
		if (bFlushScheduled)
		{
			return;
		}

		bFlushScheduled = true;

		// Defer until the per-edit transaction has closed; ReconstructNode skips work
		// while GIsTransacting.
		FlushHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([](float /*DeltaTime*/)
			{
				FlushPending();
				return false;
			}), 0.0f);
	}

	static void FlushPending()
	{
		bFlushScheduled = false;
		FlushHandle.Reset();

		if (PendingGraphs.IsEmpty())
		{
			return;
		}

		// Servicing a graph can re-enter RequestReconstruct; drain a separate set.
		TSet<TWeakObjectPtr<UPCGGraph>> Draining;
		Swap(Draining, PendingGraphs);

		++SyncStats.FlushCount;

		for (const TWeakObjectPtr<UPCGGraph>& Weak : Draining)
		{
			UPCGGraph* Graph = Weak.Get();
			if (!IsValid(Graph))
			{
				continue;   // edited graph went away before the tick; nothing to refresh
			}

			++SyncStats.GraphsFlushed;

			const EPCGReconstructResult Result = ClaireonPCGEditorSync::ReconstructOpenEditor(Graph);
			ClaireonPCGEditorSync::ReconstructOpenParentEditors(Graph);

			if (Result != EPCGReconstructResult::Unavailable)
			{
				continue;
			}

			++SyncStats.StaleViewsDetected;

			// Warn once per graph when reconstruction is unavailable.
			if (!WarnedGraphs.Contains(Graph))
			{
				WarnedGraphs.Add(Graph);
				UE_LOG(LogClaireon, Warning, TEXT("[pcg] %s: %s"), *Graph->GetPathName(),
					*ClaireonPCGEditorSync::DescribeViewState(EPCGEditorViewState::CachedButClosed));
			}
		}
	}
} // namespace ClaireonPCGEditorSync_Impl

bool ClaireonPCGEditorSync::IsReconstructAvailable()
{
	using namespace ClaireonPCGEditorSync_Impl;

	UClass* BaseClass = PCGEditorNodeBaseClass();
	return BaseClass != nullptr
		&& FindPCGNodeProperty(BaseClass) != nullptr
		&& FindPCGEditorClass(TEXT("PCGEditorGraphNode")) != nullptr;
}

EPCGEditorViewState ClaireonPCGEditorSync::GetEditorViewState(UPCGGraph* InGraph)
{
	using namespace ClaireonPCGEditorSync_Impl;

	check(IsInGameThread());

	if (IsAssetEditorOpenFor(InGraph))
	{
		return EPCGEditorViewState::EditorOpen;
	}

	return FindEditorGraph(InGraph) != nullptr
		? EPCGEditorViewState::CachedButClosed
		: EPCGEditorViewState::NoEditorGraph;
}

FString ClaireonPCGEditorSync::DescribeViewState(EPCGEditorViewState State)
{
	const bool bCanRebuild = IsReconstructAvailable();

	switch (State)
	{
	case EPCGEditorViewState::EditorOpen:
		return bCanRebuild
			? TEXT("an asset editor is open for this graph; its editor graph was rebuilt from the "
				   "runtime graph, so the edits are visible in it now.")
			: TEXT("an asset editor is open for this graph and will NOT show these edits: this engine's "
				   "PCG editor node classes do not match the names Claireon reconstructs through, so "
				   "the editor graph cannot be rebuilt. The runtime graph and the compiled-graph cache "
				   "are correct; restart the editor to view the graph.");

	case EPCGEditorViewState::CachedButClosed:
		return bCanRebuild
			? TEXT("this graph's asset editor is closed but had been opened before, so its cached editor "
				   "graph was rebuilt; reopening the asset shows the current graph.")
			: TEXT("this graph's asset editor was open earlier in this session, so a stale editor graph is "
				   "cached on the asset and this engine's node classes do not match the names Claireon "
				   "reconstructs through. The runtime graph is correct; restart the editor to view it.");

	case EPCGEditorViewState::NoEditorGraph:
	default:
		return TEXT("no editor graph is cached for this asset, so opening its asset editor now will show "
					"the current state of the graph, edits included.");
	}
}

EPCGReconstructResult ClaireonPCGEditorSync::ReconstructOpenEditor(UPCGGraph* InGraph)
{
	using namespace ClaireonPCGEditorSync_Impl;

	check(IsInGameThread());

	if (!IsValid(InGraph))
	{
		return EPCGReconstructResult::NothingToRefresh;
	}

	UEdGraph* EditorGraph = FindEditorGraph(InGraph);
	if (!IsValid(EditorGraph))
	{
		return EPCGReconstructResult::NothingToRefresh;
	}

	UClass* NodeBaseClass = PCGEditorNodeBaseClass();
	if (!IsValid(NodeBaseClass) || !FindPCGNodeProperty(NodeBaseClass))
	{
		return EPCGReconstructResult::Unavailable;
	}

	// GetNodes excludes the input and output nodes.
	TSet<UPCGNode*> WantedNodes;
	if (UPCGNode* InputNode = InGraph->GetInputNode(); IsValid(InputNode))
	{
		WantedNodes.Add(InputNode);
	}
	if (UPCGNode* OutputNode = InGraph->GetOutputNode(); IsValid(OutputNode))
	{
		WantedNodes.Add(OutputNode);
	}
	for (UPCGNode* PCGNode : InGraph->GetNodes())
	{
		if (IsValid(PCGNode))
		{
			WantedNodes.Add(PCGNode);
		}
	}

	// Preserve non-PCG nodes, including user comments.
	TMap<UPCGNode*, UEdGraphNode*> ExistingByPCGNode;
	TArray<UEdGraphNode*> ToRemove;
	for (UEdGraphNode* EdNode : EditorGraph->Nodes)
	{
		if (!IsValid(EdNode) || !EdNode->IsA(NodeBaseClass))
		{
			continue;
		}

		UPCGNode* Linked = GetLinkedPCGNode(EdNode);
		if (!IsValid(Linked) || !WantedNodes.Contains(Linked) || ExistingByPCGNode.Contains(Linked))
		{
			// Orphaned by a removal, or a duplicate of a node already accounted for.
			ToRemove.Add(EdNode);
			continue;
		}

		ExistingByPCGNode.Add(Linked, EdNode);
	}

	// Resolve classes and construct replacements before removing existing nodes.
	// Pending nodes are not in the graph's Nodes array; mark them as garbage on failure.
	struct FPendingEditorNode
	{
		UPCGNode* PCGNode = nullptr;
		UEdGraphNode* EdNode = nullptr;
	};
	TArray<FPendingEditorNode> Pending;

	auto AbandonPending = [&Pending]()
	{
		for (const FPendingEditorNode& Entry : Pending)
		{
			if (IsValid(Entry.EdNode))
			{
				Entry.EdNode->MarkAsGarbage();
			}
		}
		Pending.Reset();
	};

	for (UPCGNode* PCGNode : WantedNodes)
	{
		if (ExistingByPCGNode.Contains(PCGNode))
		{
			continue;
		}

		const TCHAR* WantedClassName = EditorNodeClassNameFor(InGraph, PCGNode);
		UClass* NodeClass = FindPCGEditorClass(WantedClassName);
		if (!IsValid(NodeClass))
		{
			NodeClass = FindPCGEditorClass(TEXT("PCGEditorGraphNode"));
		}
		if (!IsValid(NodeClass))
		{
			AbandonPending();
			UE_LOG(LogClaireon, Warning,
				TEXT("[pcg] editor rebuild for %s is unavailable: neither %s nor PCGEditorGraphNode ")
				TEXT("resolved as an editor node class. The view was left unchanged."),
				*InGraph->GetPathName(), WantedClassName);
			return EPCGReconstructResult::Unavailable;
		}

		UEdGraphNode* NewNode = NewObject<UEdGraphNode>(EditorGraph, NodeClass, NAME_None, RF_Transactional);
		if (!IsValid(NewNode) || !SetLinkedPCGNode(NewNode, PCGNode))
		{
			if (IsValid(NewNode))
			{
				NewNode->MarkAsGarbage();
			}
			AbandonPending();
			UE_LOG(LogClaireon, Warning,
				TEXT("[pcg] editor rebuild for %s is unavailable: could not construct or link a %s. ")
				TEXT("The view was left unchanged."),
				*InGraph->GetPathName(), *NodeClass->GetName());
			return EPCGReconstructResult::Unavailable;
		}

		Pending.Add({PCGNode, NewNode});
	}

	// All fallible preflight work is complete; begin reconciliation.
	int32 Removed = 0;
	for (UEdGraphNode* EdNode : ToRemove)
	{
		UnbindBridgesForEdNode(EdNode);
		EditorGraph->RemoveNode(EdNode);
		++Removed;
	}

	int32 Created = 0;
	for (const FPendingEditorNode& Entry : Pending)
	{
		UPCGNode* PCGNode = Entry.PCGNode;
		UEdGraphNode* NewNode = Entry.EdNode;

		// Use display-only fallback positions for nodes at (0,0); do not dirty the runtime asset.
		int32 PosX = PCGNode->PositionX;
		int32 PosY = PCGNode->PositionY;
		if (PosX == 0 && PosY == 0)
		{
			ClaireonPCGGraphHelpers::ComputeDisplayPosition(InGraph, PCGNode, PosX, PosY);
		}
		NewNode->NodePosX = PosX;
		NewNode->NodePosY = PosY;
		NewNode->CreateNewGuid();

		EditorGraph->AddNode(NewNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);

		// Allocate pins before reconstructing any links: the unordered reconstruction pass
		// may visit a node before its peer, and a peer without pins fails link resolution.
		NewNode->AllocateDefaultPins();

		BindSynthesizedNodeBridge(PCGNode, NewNode);

		ExistingByPCGNode.Add(PCGNode, NewNode);
		++Created;
	}

	// All peer nodes and pins must exist before PCG reconstructs links.
	for (const TPair<UPCGNode*, UEdGraphNode*>& Pair : ExistingByPCGNode)
	{
		if (Pair.Value)
		{
			Pair.Value->ReconstructNode();
		}
	}

	EditorGraph->NotifyGraphChanged();

	++SyncStats.Reconstructed;
	UE_LOG(LogClaireon, Verbose,
		TEXT("[pcg] rebuilt editor graph for %s (%d node(s) created, %d removed, %d reconstructed)"),
		*InGraph->GetPathName(), Created, Removed, ExistingByPCGNode.Num());

	return EPCGReconstructResult::Reconstructed;
}

int32 ClaireonPCGEditorSync::ReconstructOpenParentEditors(UPCGGraph* InGraph)
{
	check(IsInGameThread());

	if (!IsValid(InGraph) || !IsReconstructAvailable())
	{
		return 0;
	}

	const UPackage* Package = InGraph->GetOutermost();
	if (!IsValid(Package))
	{
		return 0;
	}

	IAssetRegistry* Registry = IAssetRegistry::Get();
	if (!Registry)
	{
		return 0;
	}

	TArray<FName> Referencers;
	Registry->GetReferencers(Package->GetFName(), Referencers);

	int32 Reconstructed = 0;
	int32 Skipped = 0;

	for (const FName& ReferencerPackageName : Referencers)
	{
		if (Reconstructed >= MaxParentsPerFlush)
		{
			++Skipped;
			continue;
		}

		// Refresh loaded parents only; do not load assets as an edit side effect.
		UPackage* ReferencerPackage = FindPackage(nullptr, *ReferencerPackageName.ToString());
		if (!IsValid(ReferencerPackage))
		{
			continue;
		}

		ForEachObjectWithPackage(ReferencerPackage, [InGraph, &Reconstructed](UObject* Object)
		{
			UPCGGraph* ParentGraph = Cast<UPCGGraph>(Object);
			if (IsValid(ParentGraph) && ParentGraph != InGraph && Reconstructed < MaxParentsPerFlush)
			{
				if (ReconstructOpenEditor(ParentGraph) == EPCGReconstructResult::Reconstructed)
				{
					++Reconstructed;
				}
			}
			return true;
		}, ClaireonPCGEditorSync_Impl::GNoNestedObjects);
	}

	if (Skipped > 0)
	{
		UE_LOG(LogClaireon, Verbose,
			TEXT("[pcg] parent-graph rebuild capped at %d for %s; %d referencing package(s) not visited"),
			MaxParentsPerFlush, *InGraph->GetPathName(), Skipped);
	}

	return Reconstructed;
}

void ClaireonPCGEditorSync::RequestReconstruct(UPCGGraph* InGraph)
{
	using namespace ClaireonPCGEditorSync_Impl;

	if (!IsValid(InGraph) || !IsInGameThread())
	{
		return;
	}

	PendingGraphs.Add(InGraph);
	EnsureFlushScheduled();
}

void ClaireonPCGEditorSync::SettlePendingReconstruct(UPCGGraph* InGraph)
{
	using namespace ClaireonPCGEditorSync_Impl;

	if (!IsValid(InGraph) || !IsInGameThread() || !PendingGraphs.Contains(InGraph))
	{
		return;
	}
	// Flush the entire pending set, including other queued graphs.
	FlushPendingNow();
}

void ClaireonPCGEditorSync::CancelPendingReconstruct(UPCGGraph* InGraph)
{
	using namespace ClaireonPCGEditorSync_Impl;

	if (IsValid(InGraph))
	{
		PendingGraphs.Remove(InGraph);
	}
}

void ClaireonPCGEditorSync::ShutdownReconstructScheduler()
{
	using namespace ClaireonPCGEditorSync_Impl;

	// Unregister before module unload to avoid callbacks into freed code.
	if (FlushHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(FlushHandle);
		FlushHandle.Reset();
	}

	// Runtime nodes can outlive the module; unbind their lambdas before unload.
	for (TPair<FObjectKey, FSynthesizedNodeBridge>& Pair : SynthesizedNodeBridges)
	{
		if (UPCGNode* Node = Pair.Value.PCGNode.Get())
		{
			Node->OnNodeChangedDelegate.Remove(Pair.Value.Handle);
		}
	}
	SynthesizedNodeBridges.Reset();

	bFlushScheduled = false;
	PendingGraphs.Reset();
	WarnedGraphs.Reset();
}

const ClaireonPCGEditorSync::FReconstructStats& ClaireonPCGEditorSync::GetStats()
{
	return ClaireonPCGEditorSync_Impl::SyncStats;
}

void ClaireonPCGEditorSync::SetMissingEditorNodeClassForTests(FName ClassName)
{
	ClaireonPCGEditorSync_Impl::MissingEditorNodeClassForTests = ClassName;
}

void ClaireonPCGEditorSync::ResetStats()
{
	ClaireonPCGEditorSync_Impl::SyncStats = FReconstructStats();
	ClaireonPCGEditorSync_Impl::WarnedGraphs.Reset();
}

int32 ClaireonPCGEditorSync::GetPendingCount()
{
	return ClaireonPCGEditorSync_Impl::PendingGraphs.Num();
}

void ClaireonPCGEditorSync::FlushPendingNow()
{
	using namespace ClaireonPCGEditorSync_Impl;

	if (FlushHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(FlushHandle);
		FlushHandle.Reset();
	}

	FlushPending();
}
