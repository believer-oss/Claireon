// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBPSnapshot.h"

#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Engine/Blueprint.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionTerminator.h"
#include "K2Node_Knot.h"
#include "K2Node_Tunnel.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Package.h"

// Prefix helpers to avoid unity-build collisions.
namespace ClaireonBPSnapshotInternal
{

/**
 * Canonical pin type for detecting signature changes independently of links.
 * Reserve and append to avoid per-pin variadic formatting cost.
 */
FString BPSnap_CanonicalPinType(const FEdGraphPinType& PinType)
{
	FString Result;
	Result.Reserve(96);

	PinType.PinCategory.AppendString(Result);
	Result.AppendChar(TEXT('|'));
	PinType.PinSubCategory.AppendString(Result);
	Result.AppendChar(TEXT('|'));
	if (const UObject* SubObject = PinType.PinSubCategoryObject.Get(); IsValid(SubObject))
	{
		Result.Append(SubObject->GetPathName());
	}
	Result.AppendChar(TEXT('|'));
	if (PinType.PinSubCategoryMemberReference.MemberName != NAME_None)
	{
		PinType.PinSubCategoryMemberReference.MemberName.AppendString(Result);
	}

	// Fixed-width flag block. ContainerType is a small enum, so one digit is exact.
	Result.AppendChar(TEXT('|'));
	Result.AppendChar(static_cast<TCHAR>(TEXT('0') + static_cast<int32>(PinType.ContainerType)));
	Result.AppendChar(PinType.bIsReference ? TEXT('R') : TEXT('-'));
	Result.AppendChar(PinType.bIsConst ? TEXT('C') : TEXT('-'));
	Result.AppendChar(PinType.bIsWeakPointer ? TEXT('W') : TEXT('-'));

	return Result;
}

/** Canonical default across all three channels; empty means every channel is empty. */
FString BPSnap_CanonicalPinDefault(const UEdGraphPin& Pin)
{
	const bool bHasText = !Pin.DefaultTextValue.IsEmpty();
	if (Pin.DefaultValue.IsEmpty() && Pin.DefaultObject == nullptr && !bHasText)
	{
		return FString();
	}

	FString Result;
	Result.Reserve(Pin.DefaultValue.Len() + 32);
	Result.Append(Pin.DefaultValue);
	Result.AppendChar(TEXT('|'));
	if (Pin.DefaultObject)
	{
		Result.Append(Pin.DefaultObject->GetPathName());
	}
	Result.AppendChar(TEXT('|'));
	if (bHasText)
	{
		Result.Append(Pin.DefaultTextValue.ToString());
	}
	return Result;
}

/** Map-key formatting, including FName unsupported by generic lexical conversion. */
FString BPSnap_KeyToString(const FGuid& Key)
{
	return Key.ToString(EGuidFormats::DigitsWithHyphens);
}

FString BPSnap_KeyToString(const FName& Key)
{
	return Key.ToString();
}

/** Stable, sortable text for one link endpoint. Identity only -- never a title. */
FString BPSnap_EndpointToString(const FClaireonBPLinkEndpoint& Endpoint)
{
	return FString::Printf(TEXT("%s/%s"),
		*Endpoint.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
		*Endpoint.PinId.ToString(EGuidFormats::DigitsWithHyphens));
}

/** Capture detail for changed-mode display or full durable-effect evidence. */
enum class EBPSnapPinDetail : uint8
{
	/** Identity, orphan flag, and links only. */
	IdentityAndLinks,

	/** Adds the canonical pin type and the canonical default across all three channels. */
	Full,
};

/** One pin, by value. */
void BPSnap_CapturePin(const UEdGraphPin& Pin, EBPSnapPinDetail Detail, FClaireonBPPinSnapshot& OutPin)
{
	OutPin.PinId = Pin.PinId;
	OutPin.PinName = Pin.PinName;
	OutPin.bIsInput = (Pin.Direction == EEdGraphPinDirection::EGPD_Input);

	// Record exec category even when changed-diff capture omits PinType.
	OutPin.bIsExec = (Pin.PinType.PinCategory == UEdGraphSchema_K2::PC_Exec);

	// Reconstruction can preserve unmatched PinIds as orphans; presence alone misses the change.
	OutPin.bOrphaned = Pin.bOrphanedPin;

	if (Detail == EBPSnapPinDetail::Full)
	{
		OutPin.PinType = BPSnap_CanonicalPinType(Pin.PinType);
		OutPin.DefaultValue = BPSnap_CanonicalPinDefault(Pin);
	}

	OutPin.Links.Reserve(Pin.LinkedTo.Num());
	for (const UEdGraphPin* Linked : Pin.LinkedTo)
	{
		if (!Linked)
		{
			continue;
		}
		const UEdGraphNode* OwningNode = Linked->GetOwningNodeUnchecked();
		if (!IsValid(OwningNode))
		{
			continue;
		}

		FClaireonBPLinkEndpoint Endpoint;
		Endpoint.NodeGuid = OwningNode->NodeGuid;
		Endpoint.PinId = Linked->PinId;
		OutPin.Links.Add(Endpoint);
	}
}

/** Capture topology in one walk, sharing a class-path cache to avoid repeated allocation. */
void BPSnap_CaptureGraph(
	const UEdGraph& Graph,
	EBPSnapPinDetail Detail,
	TMap<const UClass*, FString>& ClassPathCache,
	FClaireonBPGraphTopologySnapshot& OutGraph)
{
	OutGraph.GraphGuid = Graph.GraphGuid;
	OutGraph.GraphName = Graph.GetFName();
	OutGraph.Nodes.Reserve(Graph.Nodes.Num());

	for (const UEdGraphNode* Node : Graph.Nodes)
	{
		if (!IsValid(Node))
		{
			continue;
		}

		FClaireonBPNodeSnapshot NodeSnapshot;
		NodeSnapshot.NodeGuid = Node->NodeGuid;

		// Use class identity rather than titles so renames do not appear as rewires.
		const UClass* NodeClass = Node->GetClass();
		if (const FString* Cached = ClassPathCache.Find(NodeClass))
		{
			NodeSnapshot.NodeClassPath = *Cached;
		}
		else
		{
			NodeSnapshot.NodeClassPath = NodeClass->GetPathName();
			ClassPathCache.Add(NodeClass, NodeSnapshot.NodeClassPath);
		}

		NodeSnapshot.Pins.Reserve(Node->Pins.Num());
		for (const UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin)
			{
				continue;
			}
			FClaireonBPPinSnapshot PinSnapshot;
			BPSnap_CapturePin(*Pin, Detail, PinSnapshot);
			NodeSnapshot.Pins.Add(PinSnapshot.PinId, MoveTemp(PinSnapshot));
		}

		OutGraph.Nodes.Add(NodeSnapshot.NodeGuid, MoveTemp(NodeSnapshot));
	}
}

void BPSnap_CaptureGraphs(
	const TArray<UEdGraph*>& Graphs,
	EBPSnapPinDetail Detail,
	TMap<FGuid, FClaireonBPGraphTopologySnapshot>& OutGraphs)
{
	OutGraphs.Reserve(Graphs.Num());
	TMap<const UClass*, FString> ClassPathCache;
	for (const UEdGraph* Graph : Graphs)
	{
		if (!IsValid(Graph))
		{
			continue;
		}
		FClaireonBPGraphTopologySnapshot GraphSnapshot;
		BPSnap_CaptureGraph(*Graph, Detail, ClassPathCache, GraphSnapshot);
		OutGraphs.Add(GraphSnapshot.GraphGuid, MoveTemp(GraphSnapshot));
	}
}

/**
 * Capture class identities and Blueprint state, including skeleton function names
 * to detect changes when regeneration preserves the class pointer.
 */
void BPSnap_CaptureGeneratedClass(const UBlueprint& Blueprint, FClaireonBPGeneratedClassSnapshot& Out)
{
	const UClass* SkeletonClass = Blueprint.SkeletonGeneratedClass.Get();
	const UClass* GeneratedClass = Blueprint.GeneratedClass.Get();

	Out.SkeletonGeneratedClassIdentity = static_cast<uint64>(reinterpret_cast<UPTRINT>(SkeletonClass));
	Out.GeneratedClassIdentity = static_cast<uint64>(reinterpret_cast<UPTRINT>(GeneratedClass));
	Out.SkeletonGeneratedClassPath = IsValid(SkeletonClass) ? SkeletonClass->GetPathName() : FString();
	Out.GeneratedClassPath = IsValid(GeneratedClass) ? GeneratedClass->GetPathName() : FString();
	Out.BlueprintGuid = Blueprint.GetBlueprintGuid();

	// Capture the skeleton's own function set because regeneration may preserve its pointer.
	Out.SkeletonFunctionNames.Reset();
	if (SkeletonClass != nullptr)
	{
		for (TFieldIterator<UFunction> It(SkeletonClass, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (const UFunction* Function = *It; IsValid(Function))
			{
				Out.SkeletonFunctionNames.Add(Function->GetName());
			}
		}
		Out.SkeletonFunctionNames.Sort();
	}

#if WITH_EDITORONLY_DATA
	Out.Status = static_cast<uint8>(Blueprint.Status.GetValue());
	Out.bCachedDependenciesUpToDate = Blueprint.bCachedDependenciesUpToDate;
#endif
}

/** Capture package dirty state and persistent GUID; no per-edit revision is available. */
void BPSnap_CapturePackage(const UBlueprint& Blueprint, FClaireonBPPackageSnapshot& Out)
{
	const UPackage* Package = Blueprint.GetOutermost();
	if (!IsValid(Package))
	{
		return;
	}
	Out.PackageName = Package->GetName();
	Out.bIsDirty = Package->IsDirty();
	Out.PersistentGuid = Package->GetPersistentGuid();
}

/**
 * Capture head-transaction identity; mark unobservable if the buffer is absent.
 * Queue length is diagnostic only because eviction can change it independently.
 */
void BPSnap_CaptureTransactions(FClaireonBPTransactionSnapshot& Out)
{
	if (!IsValid(GEditor) || !GEditor->Trans)
	{
		Out.bObservable = false;
		return;
	}

	UTransactor* Trans = GEditor->Trans;
	Out.bObservable = true;
	Out.QueueLength = Trans->GetQueueLength();

	// The head is the most recent transaction that has not been undone.
	const int32 HeadIndex = Out.QueueLength - Trans->GetUndoCount() - 1;
	if (HeadIndex >= 0 && HeadIndex < Out.QueueLength)
	{
		if (const FTransaction* Head = Trans->GetTransaction(HeadIndex))
		{
			Out.HeadTransactionId = Head->GetId();
			Out.HeadTransactionOperationId = Head->GetOperationId();
			Out.HeadTransactionTitle = Head->GetTitle().ToString();
		}
	}

	// Capture outside tool-owned transactions so an active group belongs to the caller.
	Out.bCallerGroupActive = Trans->IsActive();

	// The caller supplies CallerGroupLabel and ClaireonOwnedTitles; the buffer does not identify ownership.
}

/** Every function-entry node in the supplied graphs. Bounded by the one walk. */
void BPSnap_ForEachFunctionEntry(
	const TArray<UEdGraph*>& Graphs,
	TFunctionRef<void(const UEdGraph&, const UK2Node_FunctionEntry&)> Visitor)
{
	for (const UEdGraph* Graph : Graphs)
	{
		if (!IsValid(Graph))
		{
			continue;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (const UK2Node_FunctionEntry* Entry = Cast<UK2Node_FunctionEntry>(Node); IsValid(Entry))
			{
				Visitor(*Graph, *Entry);
			}
		}
	}
}

/** The function name a given entry node declares. */
FName BPSnap_EntryFunctionName(const UEdGraph& Graph, const UK2Node_FunctionEntry& Entry)
{
	return Entry.CustomGeneratedFunctionName != NAME_None ? Entry.CustomGeneratedFunctionName : Graph.GetFName();
}

/** The access-specifier bits of a function-flag mask, as a wire string. */
FString BPSnap_AccessSpecifierString(int32 Flags)
{
	if ((Flags & FUNC_Private) != 0)
	{
		return TEXT("private");
	}
	if ((Flags & FUNC_Protected) != 0)
	{
		return TEXT("protected");
	}
	if ((Flags & FUNC_Public) != 0)
	{
		return TEXT("public");
	}
	return TEXT("unspecified");
}

/** The net bits only, so an RPC transition is legible on its own. */
uint32 BPSnap_NetFlagMask(int32 Flags)
{
	const int32 NetBits = FUNC_Net | FUNC_NetReliable | FUNC_NetRequest | FUNC_NetResponse
		| FUNC_NetMulticast | FUNC_NetServer | FUNC_NetClient | FUNC_NetValidate;
	return static_cast<uint32>(Flags & NetBits);
}

void BPSnap_CaptureExtractionProperties(const TArray<UEdGraph*>& Graphs, FClaireonBPExtractionPropertySnapshot& Out)
{
	for (const UEdGraph* Graph : Graphs)
	{
		if (!IsValid(Graph))
		{
			continue;
		}

		// A rename must read as a rename and not as a disappearance.
		Out.GraphNames.Add(Graph->GraphGuid, Graph->GetFName());

		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			// Capture both tunnel and function-terminator boundaries; their hierarchies are disjoint.
			const bool bIsGateway = IsValid(Node) && (Node->IsA<UK2Node_Tunnel>() || Node->IsA<UK2Node_FunctionTerminator>());
			if (bIsGateway)
			{
				TArray<FGuid>& PinIds = Out.GatewayPins.FindOrAdd(Node->NodeGuid);
				for (const UEdGraphPin* Pin : Node->Pins)
				{
					if (Pin)
					{
						PinIds.Add(Pin->PinId);
					}
				}
			}
		}
	}

	BPSnap_ForEachFunctionEntry(Graphs, [&Out](const UEdGraph& Graph, const UK2Node_FunctionEntry& Entry)
	{
		const FName FunctionName = BPSnap_EntryFunctionName(Graph, Entry);

		Out.FunctionFlags.Add(FunctionName, static_cast<uint32>(Entry.GetFunctionFlags()));

		TArray<FString>& Signature = Out.EntrySignatures.FindOrAdd(FunctionName);
		for (const UEdGraphPin* Pin : Entry.Pins)
		{
			if (Pin && Pin->Direction == EEdGraphPinDirection::EGPD_Output)
			{
				Signature.Add(FString::Printf(TEXT("%s:%s"),
					*Pin->PinName.ToString(),
					*BPSnap_CanonicalPinType(Pin->PinType)));
			}
		}
	});
}

void BPSnap_CaptureSetterProperties(const TArray<UEdGraph*>& Graphs, FClaireonBPSetterPropertySnapshot& Out)
{
	BPSnap_ForEachFunctionEntry(Graphs, [&Out](const UEdGraph& Graph, const UK2Node_FunctionEntry& Entry)
	{
		const FName FunctionName = BPSnap_EntryFunctionName(Graph, Entry);
		const int32 FunctionFlags = Entry.GetFunctionFlags();
		const int32 ExtraFlags = Entry.GetExtraFlags();

		Out.FunctionFlags.Add(FunctionName, static_cast<uint32>(FunctionFlags));
		Out.EntryExtraFlags.Add(FunctionName, ExtraFlags);
		Out.Categories.Add(FunctionName, Entry.MetaData.Category.ToString());
		Out.Tooltips.Add(FunctionName, Entry.MetaData.ToolTip.ToString());
		Out.AccessSpecifiers.Add(FunctionName, BPSnap_AccessSpecifierString(FunctionFlags));
		Out.NetFlagMasks.Add(FunctionName, BPSnap_NetFlagMask(FunctionFlags));
	});
}

void BPSnap_CaptureFormatProperties(const TArray<UEdGraph*>& Graphs, FClaireonBPFormatPropertySnapshot& Out)
{
	for (const UEdGraph* Graph : Graphs)
	{
		if (!IsValid(Graph))
		{
			continue;
		}
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (!IsValid(Node))
			{
				continue;
			}

			// Capture positions only for the format family; asynchronous layout is not extraction evidence.
			Out.NodePositions.Add(Node->NodeGuid, FIntPoint(Node->NodePosX, Node->NodePosY));

			if (const UEdGraphNode_Comment* Comment = Cast<UEdGraphNode_Comment>(Node); IsValid(Comment))
			{
				Out.CommentBounds.Add(Comment->NodeGuid, FIntRect(
					Comment->NodePosX,
					Comment->NodePosY,
					Comment->NodePosX + Comment->NodeWidth,
					Comment->NodePosY + Comment->NodeHeight));

				// Capture comment containment separately from semantic topology.
				TArray<FGuid>& Contained = Out.CommentContainment.FindOrAdd(Comment->NodeGuid);
				for (const UObject* Under : Comment->GetNodesUnderComment())
				{
					if (const UEdGraphNode* UnderNode = Cast<UEdGraphNode>(Under); IsValid(UnderNode))
					{
						Contained.Add(UnderNode->NodeGuid);
					}
				}
			}

			if (Node->IsA<UK2Node_Knot>())
			{
				Out.KnotNodes.Add(Node->NodeGuid);
			}
		}
	}
}

// Diff helpers.

void BPSnap_AddEntry(
	FClaireonBPSnapshotDelta& Delta,
	EClaireonBPDurableEffectClass EffectClass,
	EClaireonBPDeltaKind Kind,
	FString&& Target,
	FString&& Before,
	FString&& After)
{
	FClaireonBPDeltaEntry Entry;
	Entry.EffectClass = EffectClass;
	Entry.Kind = Kind;
	Entry.Target = MoveTemp(Target);
	Entry.Before = MoveTemp(Before);
	Entry.After = MoveTemp(After);
	Delta.Entries.Add(MoveTemp(Entry));
}

/** Report a scalar that moved, and nothing when it did not. */
void BPSnap_DiffScalar(
	FClaireonBPSnapshotDelta& Delta,
	EClaireonBPDurableEffectClass EffectClass,
	const TCHAR* Target,
	const FString& Before,
	const FString& After)
{
	if (Before != After)
	{
		BPSnap_AddEntry(Delta, EffectClass, EClaireonBPDeltaKind::Changed, Target, CopyTemp(Before), CopyTemp(After));
	}
}

/** Two maps of value-comparable entries, diffed into Added / Removed / Changed. */
template <typename KeyType, typename ValueType, typename ToStringType>
void BPSnap_DiffMap(
	FClaireonBPSnapshotDelta& Delta,
	EClaireonBPDurableEffectClass EffectClass,
	const TCHAR* TargetPrefix,
	const TMap<KeyType, ValueType>& Before,
	const TMap<KeyType, ValueType>& After,
	ToStringType&& ValueToString)
{
	for (const TPair<KeyType, ValueType>& Pair : Before)
	{
		const ValueType* AfterValue = After.Find(Pair.Key);
		FString Target = FString::Printf(TEXT("%s%s"), TargetPrefix, *BPSnap_KeyToString(Pair.Key));
		if (!AfterValue)
		{
			BPSnap_AddEntry(Delta, EffectClass, EClaireonBPDeltaKind::Removed,
				MoveTemp(Target), ValueToString(Pair.Value), FString());
		}
		else
		{
			FString BeforeText = ValueToString(Pair.Value);
			FString AfterText = ValueToString(*AfterValue);
			if (BeforeText != AfterText)
			{
				BPSnap_AddEntry(Delta, EffectClass, EClaireonBPDeltaKind::Changed,
					MoveTemp(Target), MoveTemp(BeforeText), MoveTemp(AfterText));
			}
		}
	}
	for (const TPair<KeyType, ValueType>& Pair : After)
	{
		if (!Before.Contains(Pair.Key))
		{
			BPSnap_AddEntry(Delta, EffectClass, EClaireonBPDeltaKind::Added,
				FString::Printf(TEXT("%s%s"), TargetPrefix, *BPSnap_KeyToString(Pair.Key)),
				FString(), ValueToString(Pair.Value));
		}
	}
}

void BPSnap_DiffPin(
	FClaireonBPSnapshotDelta& Delta,
	const FGuid& NodeGuid,
	const FClaireonBPPinSnapshot& Before,
	const FClaireonBPPinSnapshot& After)
{
	const FString PinTarget = FString::Printf(TEXT("pin:%s/%s"),
		*NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
		*Before.PinId.ToString(EGuidFormats::DigitsWithHyphens));

	if (Before.PinName != After.PinName)
	{
		BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Changed,
			PinTarget + TEXT("#name"), Before.PinName.ToString(), After.PinName.ToString());
	}
	if (Before.PinType != After.PinType)
	{
		BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Changed,
			PinTarget + TEXT("#type"), CopyTemp(Before.PinType), CopyTemp(After.PinType));
	}
	if (Before.DefaultValue != After.DefaultValue)
	{
		BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Changed,
			PinTarget + TEXT("#default"), CopyTemp(Before.DefaultValue), CopyTemp(After.DefaultValue));
	}

	// A pin can retain identity and links while becoming orphaned.
	if (Before.bOrphaned != After.bOrphaned)
	{
		BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Changed,
			PinTarget + TEXT("#orphaned"),
			Before.bOrphaned ? TEXT("true") : TEXT("false"),
			After.bOrphaned ? TEXT("true") : TEXT("false"));
	}

	TSet<FString> BeforeLinks;
	BeforeLinks.Reserve(Before.Links.Num());
	for (const FClaireonBPLinkEndpoint& Endpoint : Before.Links)
	{
		BeforeLinks.Add(BPSnap_EndpointToString(Endpoint));
	}

	TSet<FString> AfterLinks;
	AfterLinks.Reserve(After.Links.Num());
	for (const FClaireonBPLinkEndpoint& Endpoint : After.Links)
	{
		AfterLinks.Add(BPSnap_EndpointToString(Endpoint));
	}

	for (const FString& Link : BeforeLinks)
	{
		if (!AfterLinks.Contains(Link))
		{
			BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Removed,
				FString::Printf(TEXT("link:%s/%s"),
					*NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
					*Before.PinId.ToString(EGuidFormats::DigitsWithHyphens)),
				CopyTemp(Link), FString());
		}
	}
	for (const FString& Link : AfterLinks)
	{
		if (!BeforeLinks.Contains(Link))
		{
			BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Added,
				FString::Printf(TEXT("link:%s/%s"),
					*NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
					*After.PinId.ToString(EGuidFormats::DigitsWithHyphens)),
				FString(), CopyTemp(Link));
		}
	}
}

void BPSnap_DiffNode(
	FClaireonBPSnapshotDelta& Delta,
	const FClaireonBPNodeSnapshot& Before,
	const FClaireonBPNodeSnapshot& After)
{
	const FString NodeTarget = FString::Printf(TEXT("node:%s"),
		*Before.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));

	if (Before.NodeClassPath != After.NodeClassPath)
	{
		BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Changed,
			NodeTarget + TEXT("#class"), CopyTemp(Before.NodeClassPath), CopyTemp(After.NodeClassPath));
	}

	for (const TPair<FGuid, FClaireonBPPinSnapshot>& Pair : Before.Pins)
	{
		if (const FClaireonBPPinSnapshot* AfterPin = After.Pins.Find(Pair.Key))
		{
			BPSnap_DiffPin(Delta, Before.NodeGuid, Pair.Value, *AfterPin);
		}
		else
		{
			BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Removed,
				FString::Printf(TEXT("pin:%s/%s"),
					*Before.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
					*Pair.Key.ToString(EGuidFormats::DigitsWithHyphens)),
				Pair.Value.PinName.ToString(), FString());
		}
	}
	for (const TPair<FGuid, FClaireonBPPinSnapshot>& Pair : After.Pins)
	{
		if (!Before.Pins.Contains(Pair.Key))
		{
			BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Added,
				FString::Printf(TEXT("pin:%s/%s"),
					*After.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
					*Pair.Key.ToString(EGuidFormats::DigitsWithHyphens)),
				FString(), Pair.Value.PinName.ToString());
		}
	}
}

void BPSnap_DiffGraph(
	FClaireonBPSnapshotDelta& Delta,
	const FClaireonBPGraphTopologySnapshot& Before,
	const FClaireonBPGraphTopologySnapshot& After)
{
	if (Before.GraphName != After.GraphName)
	{
		BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Changed,
			FString::Printf(TEXT("graph:%s#name"), *Before.GraphGuid.ToString(EGuidFormats::DigitsWithHyphens)),
			Before.GraphName.ToString(), After.GraphName.ToString());
	}

	for (const TPair<FGuid, FClaireonBPNodeSnapshot>& Pair : Before.Nodes)
	{
		if (const FClaireonBPNodeSnapshot* AfterNode = After.Nodes.Find(Pair.Key))
		{
			BPSnap_DiffNode(Delta, Pair.Value, *AfterNode);
		}
		else
		{
			BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Removed,
				FString::Printf(TEXT("node:%s"), *Pair.Key.ToString(EGuidFormats::DigitsWithHyphens)),
				CopyTemp(Pair.Value.NodeClassPath), FString());
		}
	}
	for (const TPair<FGuid, FClaireonBPNodeSnapshot>& Pair : After.Nodes)
	{
		if (!Before.Nodes.Contains(Pair.Key))
		{
			BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Added,
				FString::Printf(TEXT("node:%s"), *Pair.Key.ToString(EGuidFormats::DigitsWithHyphens)),
				FString(), CopyTemp(Pair.Value.NodeClassPath));
		}
	}
}

void BPSnap_DiffTopology(FClaireonBPSnapshotDelta& Delta, const FClaireonBPSnapshot& Before, const FClaireonBPSnapshot& After)
{
	for (const TPair<FGuid, FClaireonBPGraphTopologySnapshot>& Pair : Before.Graphs)
	{
		if (const FClaireonBPGraphTopologySnapshot* AfterGraph = After.Graphs.Find(Pair.Key))
		{
			BPSnap_DiffGraph(Delta, Pair.Value, *AfterGraph);
		}
		else
		{
			BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Removed,
				FString::Printf(TEXT("graph:%s"), *Pair.Key.ToString(EGuidFormats::DigitsWithHyphens)),
				Pair.Value.GraphName.ToString(), FString());
		}
	}
	for (const TPair<FGuid, FClaireonBPGraphTopologySnapshot>& Pair : After.Graphs)
	{
		if (!Before.Graphs.Contains(Pair.Key))
		{
			BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GraphTopology, EClaireonBPDeltaKind::Added,
				FString::Printf(TEXT("graph:%s"), *Pair.Key.ToString(EGuidFormats::DigitsWithHyphens)),
				FString(), Pair.Value.GraphName.ToString());
		}
	}
}

void BPSnap_DiffGeneratedClass(FClaireonBPSnapshotDelta& Delta, const FClaireonBPGeneratedClassSnapshot& Before, const FClaireonBPGeneratedClassSnapshot& After)
{
	// Compare class pointers; paths identify changes but can survive regeneration.
	if (Before.SkeletonGeneratedClassIdentity != After.SkeletonGeneratedClassIdentity)
	{
		BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GeneratedClass, EClaireonBPDeltaKind::Changed,
			TEXT("skeleton_generated_class_identity"),
			FString::Printf(TEXT("%s@%llu"), *Before.SkeletonGeneratedClassPath, Before.SkeletonGeneratedClassIdentity),
			FString::Printf(TEXT("%s@%llu"), *After.SkeletonGeneratedClassPath, After.SkeletonGeneratedClassIdentity));
	}
	if (Before.GeneratedClassIdentity != After.GeneratedClassIdentity)
	{
		BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GeneratedClass, EClaireonBPDeltaKind::Changed,
			TEXT("generated_class_identity"),
			FString::Printf(TEXT("%s@%llu"), *Before.GeneratedClassPath, Before.GeneratedClassIdentity),
			FString::Printf(TEXT("%s@%llu"), *After.GeneratedClassPath, After.GeneratedClassIdentity));
	}

	// Emit individual changed function names rather than entire sets.
	{
		const TSet<FString> BeforeSet(Before.SkeletonFunctionNames);
		const TSet<FString> AfterSet(After.SkeletonFunctionNames);

		TArray<FString> Added = AfterSet.Difference(BeforeSet).Array();
		TArray<FString> Removed = BeforeSet.Difference(AfterSet).Array();
		Added.Sort();
		Removed.Sort();

		for (const FString& Name : Added)
		{
			BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GeneratedClass,
				EClaireonBPDeltaKind::Added,
				FString::Printf(TEXT("skeleton_function:%s"), *Name), FString(), CopyTemp(Name));
		}
		for (const FString& Name : Removed)
		{
			BPSnap_AddEntry(Delta, EClaireonBPDurableEffectClass::GeneratedClass,
				EClaireonBPDeltaKind::Removed,
				FString::Printf(TEXT("skeleton_function:%s"), *Name), CopyTemp(Name), FString());
		}
	}

	BPSnap_DiffScalar(Delta, EClaireonBPDurableEffectClass::GeneratedClass, TEXT("blueprint_guid"),
		Before.BlueprintGuid.ToString(EGuidFormats::DigitsWithHyphens),
		After.BlueprintGuid.ToString(EGuidFormats::DigitsWithHyphens));
	BPSnap_DiffScalar(Delta, EClaireonBPDurableEffectClass::GeneratedClass, TEXT("blueprint_status"),
		FString::FromInt(Before.Status), FString::FromInt(After.Status));
	BPSnap_DiffScalar(Delta, EClaireonBPDurableEffectClass::GeneratedClass, TEXT("cached_dependencies_up_to_date"),
		Before.bCachedDependenciesUpToDate ? TEXT("true") : TEXT("false"),
		After.bCachedDependenciesUpToDate ? TEXT("true") : TEXT("false"));
}

void BPSnap_DiffPackage(FClaireonBPSnapshotDelta& Delta, const FClaireonBPPackageSnapshot& Before, const FClaireonBPPackageSnapshot& After)
{
	BPSnap_DiffScalar(Delta, EClaireonBPDurableEffectClass::PackageState, TEXT("package_name"),
		Before.PackageName, After.PackageName);
	BPSnap_DiffScalar(Delta, EClaireonBPDurableEffectClass::PackageState, TEXT("package_dirty"),
		Before.bIsDirty ? TEXT("true") : TEXT("false"),
		After.bIsDirty ? TEXT("true") : TEXT("false"));
	BPSnap_DiffScalar(Delta, EClaireonBPDurableEffectClass::PackageState, TEXT("package_persistent_guid"),
		Before.PersistentGuid.ToString(EGuidFormats::DigitsWithHyphens),
		After.PersistentGuid.ToString(EGuidFormats::DigitsWithHyphens));
}

void BPSnap_DiffTransactions(FClaireonBPSnapshotDelta& Delta, const FClaireonBPTransactionSnapshot& Before, const FClaireonBPTransactionSnapshot& After)
{
	// Compare head GUIDs; eviction and redo truncation make queue length unattributable.
	BPSnap_DiffScalar(Delta, EClaireonBPDurableEffectClass::TransactionBuffer, TEXT("head_transaction_id"),
		Before.HeadTransactionId.ToString(EGuidFormats::DigitsWithHyphens),
		After.HeadTransactionId.ToString(EGuidFormats::DigitsWithHyphens));
	BPSnap_DiffScalar(Delta, EClaireonBPDurableEffectClass::TransactionBuffer, TEXT("head_transaction_operation_id"),
		Before.HeadTransactionOperationId.ToString(EGuidFormats::DigitsWithHyphens),
		After.HeadTransactionOperationId.ToString(EGuidFormats::DigitsWithHyphens));
	BPSnap_DiffScalar(Delta, EClaireonBPDurableEffectClass::TransactionBuffer, TEXT("caller_group_active"),
		Before.bCallerGroupActive ? TEXT("true") : TEXT("false"),
		After.bCallerGroupActive ? TEXT("true") : TEXT("false"));
}

FString BPSnap_JoinStrings(const TArray<FString>& Values)
{
	return FString::Join(Values, TEXT(","));
}

void BPSnap_DiffFamilyProperties(FClaireonBPSnapshotDelta& Delta, const FClaireonBPSnapshot& Before, const FClaireonBPSnapshot& After)
{
	const EClaireonBPDurableEffectClass Class = EClaireonBPDurableEffectClass::FamilyProperties;

	switch (Before.Family)
	{
	case EClaireonBPSnapshotFamily::Extraction:
	{
		const FClaireonBPExtractionPropertySnapshot& B = Before.ExtractionProperties;
		const FClaireonBPExtractionPropertySnapshot& A = After.ExtractionProperties;
		BPSnap_DiffMap(Delta, Class, TEXT("function_flags:"), B.FunctionFlags, A.FunctionFlags,
			[](uint32 Value) { return FString::Printf(TEXT("0x%08X"), Value); });
		BPSnap_DiffMap(Delta, Class, TEXT("entry_signature:"), B.EntrySignatures, A.EntrySignatures,
			[](const TArray<FString>& Value) { return BPSnap_JoinStrings(Value); });
		BPSnap_DiffMap(Delta, Class, TEXT("gateway_pins:"), B.GatewayPins, A.GatewayPins,
			[](const TArray<FGuid>& Value)
			{
				TArray<FString> Text;
				Text.Reserve(Value.Num());
				for (const FGuid& Guid : Value)
				{
					Text.Add(Guid.ToString(EGuidFormats::DigitsWithHyphens));
				}
				return BPSnap_JoinStrings(Text);
			});
		BPSnap_DiffMap(Delta, Class, TEXT("graph_name:"), B.GraphNames, A.GraphNames,
			[](const FName& Value) { return Value.ToString(); });
		break;
	}
	case EClaireonBPSnapshotFamily::Setter:
	{
		const FClaireonBPSetterPropertySnapshot& B = Before.SetterProperties;
		const FClaireonBPSetterPropertySnapshot& A = After.SetterProperties;
		BPSnap_DiffMap(Delta, Class, TEXT("function_flags:"), B.FunctionFlags, A.FunctionFlags,
			[](uint32 Value) { return FString::Printf(TEXT("0x%08X"), Value); });
		BPSnap_DiffMap(Delta, Class, TEXT("entry_extra_flags:"), B.EntryExtraFlags, A.EntryExtraFlags,
			[](int32 Value) { return FString::Printf(TEXT("0x%08X"), static_cast<uint32>(Value)); });
		BPSnap_DiffMap(Delta, Class, TEXT("category:"), B.Categories, A.Categories,
			[](const FString& Value) { return Value; });
		BPSnap_DiffMap(Delta, Class, TEXT("tooltip:"), B.Tooltips, A.Tooltips,
			[](const FString& Value) { return Value; });
		BPSnap_DiffMap(Delta, Class, TEXT("access_specifier:"), B.AccessSpecifiers, A.AccessSpecifiers,
			[](const FString& Value) { return Value; });
		BPSnap_DiffMap(Delta, Class, TEXT("net_flags:"), B.NetFlagMasks, A.NetFlagMasks,
			[](uint32 Value) { return FString::Printf(TEXT("0x%08X"), Value); });
		break;
	}
	case EClaireonBPSnapshotFamily::Format:
	{
		const FClaireonBPFormatPropertySnapshot& B = Before.FormatProperties;
		const FClaireonBPFormatPropertySnapshot& A = After.FormatProperties;
		BPSnap_DiffMap(Delta, Class, TEXT("node_position:"), B.NodePositions, A.NodePositions,
			[](const FIntPoint& Value) { return FString::Printf(TEXT("%d,%d"), Value.X, Value.Y); });
		BPSnap_DiffMap(Delta, Class, TEXT("comment_bounds:"), B.CommentBounds, A.CommentBounds,
			[](const FIntRect& Value)
			{
				return FString::Printf(TEXT("%d,%d,%d,%d"), Value.Min.X, Value.Min.Y, Value.Max.X, Value.Max.Y);
			});
		BPSnap_DiffMap(Delta, Class, TEXT("comment_containment:"), B.CommentContainment, A.CommentContainment,
			[](const TArray<FGuid>& Value)
			{
				TArray<FString> Text;
				Text.Reserve(Value.Num());
				for (const FGuid& Guid : Value)
				{
					Text.Add(Guid.ToString(EGuidFormats::DigitsWithHyphens));
				}
				Text.Sort();
				return BPSnap_JoinStrings(Text);
			});

		for (const FGuid& Knot : B.KnotNodes)
		{
			if (!A.KnotNodes.Contains(Knot))
			{
				BPSnap_AddEntry(Delta, Class, EClaireonBPDeltaKind::Removed,
					FString::Printf(TEXT("knot:%s"), *Knot.ToString(EGuidFormats::DigitsWithHyphens)),
					TEXT("knot"), FString());
			}
		}
		for (const FGuid& Knot : A.KnotNodes)
		{
			if (!B.KnotNodes.Contains(Knot))
			{
				BPSnap_AddEntry(Delta, Class, EClaireonBPDeltaKind::Added,
					FString::Printf(TEXT("knot:%s"), *Knot.ToString(EGuidFormats::DigitsWithHyphens)),
					FString(), TEXT("knot"));
			}
		}
		break;
	}
	}
}

} // namespace ClaireonBPSnapshotInternal


const FClaireonBPNodeSnapshot* FClaireonBPSnapshot::FindNode(const FGuid& NodeGuid) const
{
	for (const TPair<FGuid, FClaireonBPGraphTopologySnapshot>& Pair : Graphs)
	{
		if (const FClaireonBPNodeSnapshot* Node = Pair.Value.Nodes.Find(NodeGuid))
		{
			return Node;
		}
	}
	return nullptr;
}


TArray<FClaireonBPPhaseJournalEntry> FClaireonBPPhaseJournal::EntriesForPhase(const FString& Phase) const
{
	TArray<FClaireonBPPhaseJournalEntry> Matching;
	for (const FClaireonBPPhaseJournalEntry& Entry : Entries)
	{
		if (Entry.Phase == Phase)
		{
			Matching.Add(Entry);
		}
	}
	return Matching;
}

TSharedPtr<FJsonObject> FClaireonBPPhaseJournal::ToJson() const
{
	// Null means no journal; an empty object would claim an empty journal.
	if (Entries.IsEmpty())
	{
		return nullptr;
	}

	TArray<TSharedPtr<FJsonValue>> EntryValues;
	EntryValues.Reserve(Entries.Num());
	for (const FClaireonBPPhaseJournalEntry& Entry : Entries)
	{
		TSharedPtr<FJsonObject> EntryObject = MakeShared<FJsonObject>();
		EntryObject->SetStringField(TEXT("phase"), Entry.Phase);
		EntryObject->SetStringField(TEXT("action"), Entry.Action);
		if (!Entry.Target.IsEmpty())
		{
			EntryObject->SetStringField(TEXT("target"), Entry.Target);
		}
		if (!Entry.Detail.IsEmpty())
		{
			EntryObject->SetStringField(TEXT("detail"), Entry.Detail);
		}
		EntryValues.Add(MakeShared<FJsonValueObject>(EntryObject));
	}

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// The journal records intent; only snapshot differences establish observed effects.
	Root->SetStringField(TEXT("source"), TEXT("journalled_intent"));
	Root->SetNumberField(TEXT("entry_count"), Entries.Num());
	Root->SetArrayField(TEXT("entries"), EntryValues);
	return Root;
}


TSharedPtr<FJsonObject> FClaireonBPSnapshotDelta::ToJson() const
{
	// An unavailable comparison has no entry list; do not synthesize an empty delta.
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("source"), TEXT("observed_snapshot_diff"));
	Root->SetBoolField(TEXT("comparable"), bComparable);

	// Absent transaction buffers are unobservable, not empty.
	Root->SetBoolField(TEXT("transaction_class_observable"), bTransactionClassObservable);

	// A non-comparable delta conservatively reports possible durable effects.
	Root->SetBoolField(TEXT("has_durable_effect"), HasDurableEffect());

	if (!bComparable)
	{
		Root->SetStringField(TEXT("not_comparable_reason"),
			TEXT("one or both snapshots were not captured for durable-effect comparison; "
				 "the operation must resolve to the retained state"));
		return Root;
	}

	TArray<TSharedPtr<FJsonValue>> EntryValues;
	EntryValues.Reserve(Entries.Num());
	for (const FClaireonBPDeltaEntry& Entry : Entries)
	{
		TSharedPtr<FJsonObject> EntryObject = MakeShared<FJsonObject>();
		EntryObject->SetStringField(TEXT("effect_class"), ClaireonBPSnapshot::ToWireString(Entry.EffectClass));
		EntryObject->SetStringField(TEXT("kind"), ClaireonBPSnapshot::ToWireString(Entry.Kind));
		EntryObject->SetStringField(TEXT("target"), Entry.Target);
		if (!Entry.Before.IsEmpty())
		{
			EntryObject->SetStringField(TEXT("before"), Entry.Before);
		}
		if (!Entry.After.IsEmpty())
		{
			EntryObject->SetStringField(TEXT("after"), Entry.After);
		}
		EntryValues.Add(MakeShared<FJsonValueObject>(EntryObject));
	}

	Root->SetNumberField(TEXT("entry_count"), Entries.Num());
	Root->SetArrayField(TEXT("entries"), EntryValues);
	return Root;
}


namespace ClaireonBPSnapshot
{

const TCHAR* ToWireString(EClaireonBPDurableEffectClass EffectClass)
{
	switch (EffectClass)
	{
	case EClaireonBPDurableEffectClass::GraphTopology:    return TEXT("graph_topology");
	case EClaireonBPDurableEffectClass::GeneratedClass:   return TEXT("generated_class");
	case EClaireonBPDurableEffectClass::PackageState:     return TEXT("package_state");
	case EClaireonBPDurableEffectClass::TransactionBuffer:return TEXT("transaction_buffer");
	case EClaireonBPDurableEffectClass::FamilyProperties: return TEXT("family_properties");
	}
	return TEXT("graph_topology");
}

const TCHAR* ToWireString(EClaireonBPDeltaKind Kind)
{
	switch (Kind)
	{
	case EClaireonBPDeltaKind::Added:   return TEXT("added");
	case EClaireonBPDeltaKind::Removed: return TEXT("removed");
	case EClaireonBPDeltaKind::Changed: return TEXT("changed");
	}
	return TEXT("changed");
}

const TCHAR* ToWireString(EClaireonBPSnapshotFamily Family)
{
	switch (Family)
	{
	case EClaireonBPSnapshotFamily::Extraction: return TEXT("extraction");
	case EClaireonBPSnapshotFamily::Setter:     return TEXT("setter");
	case EClaireonBPSnapshotFamily::Format:     return TEXT("format");
	}
	return TEXT("extraction");
}

bool Capture(
	UBlueprint* Blueprint,
	const TArray<UEdGraph*>& Graphs,
	EClaireonBPSnapshotFamily Family,
	FClaireonBPSnapshot& OutSnapshot)
{
	using namespace ClaireonBPSnapshotInternal;

	OutSnapshot = FClaireonBPSnapshot();
	OutSnapshot.Family = Family;

	if (!IsValid(Blueprint))
	{
		return false;
	}

	BPSnap_CaptureGraphs(Graphs, EBPSnapPinDetail::Full, OutSnapshot.Graphs);
	BPSnap_CaptureGeneratedClass(*Blueprint, OutSnapshot.GeneratedClass);
	BPSnap_CapturePackage(*Blueprint, OutSnapshot.Package);
	BPSnap_CaptureTransactions(OutSnapshot.Transactions);

	// Capture positions only for formatting; asynchronous layout is not extraction evidence.
	switch (Family)
	{
	case EClaireonBPSnapshotFamily::Extraction:
		BPSnap_CaptureExtractionProperties(Graphs, OutSnapshot.ExtractionProperties);
		break;
	case EClaireonBPSnapshotFamily::Setter:
		BPSnap_CaptureSetterProperties(Graphs, OutSnapshot.SetterProperties);
		break;
	case EClaireonBPSnapshotFamily::Format:
		BPSnap_CaptureFormatProperties(Graphs, OutSnapshot.FormatProperties);
		break;
	}

	OutSnapshot.bCaptured = true;
	return true;
}

bool CaptureForChangedDiff(const TArray<UEdGraph*>& Graphs, FClaireonBPSnapshot& OutSnapshot)
{
	using namespace ClaireonBPSnapshotInternal;

	OutSnapshot = FClaireonBPSnapshot();
	OutSnapshot.bChangedDiffOnly = true;

	BPSnap_CaptureGraphs(Graphs, EBPSnapPinDetail::IdentityAndLinks, OutSnapshot.Graphs);

	OutSnapshot.bCaptured = true;
	return true;
}

FClaireonBPSnapshotDelta Diff(const FClaireonBPSnapshot& Before, const FClaireonBPSnapshot& After)
{
	using namespace ClaireonBPSnapshotInternal;

	FClaireonBPSnapshotDelta Delta;

	Delta.bTransactionClassObservable = Before.Transactions.bObservable && After.Transactions.bObservable;

	// Topology-only pairs produce display entries but cannot prove absence of durable effects.
	const bool bScopesMatch = (Before.bChangedDiffOnly == After.bChangedDiffOnly);
	const bool bFullPair = Before.bCaptured && After.bCaptured && bScopesMatch
		&& !Before.bChangedDiffOnly && Before.Family == After.Family;

	Delta.bComparable = bFullPair;

	if (!Before.bCaptured || !After.bCaptured || !bScopesMatch)
	{
		return Delta;
	}

	BPSnap_DiffTopology(Delta, Before, After);

	if (Before.bChangedDiffOnly)
	{
		return Delta;
	}

	BPSnap_DiffGeneratedClass(Delta, Before.GeneratedClass, After.GeneratedClass);
	BPSnap_DiffPackage(Delta, Before.Package, After.Package);

	if (Delta.bTransactionClassObservable)
	{
		BPSnap_DiffTransactions(Delta, Before.Transactions, After.Transactions);
	}

	if (Before.Family == After.Family)
	{
		BPSnap_DiffFamilyProperties(Delta, Before, After);
	}

	return Delta;
}

} // namespace ClaireonBPSnapshot

// Test-only fault injection.

#if WITH_CLAIREON_TESTS

namespace ClaireonBPFaultInjectionInternal
{
	FString BPFault_ArmedPhase;
}

namespace ClaireonBPFaultInjection
{

void Arm(const FString& PhaseWireString)
{
	ClaireonBPFaultInjectionInternal::BPFault_ArmedPhase = PhaseWireString;
}

void Disarm()
{
	ClaireonBPFaultInjectionInternal::BPFault_ArmedPhase.Reset();
}

const FString& GetArmedPhase()
{
	return ClaireonBPFaultInjectionInternal::BPFault_ArmedPhase;
}

bool ShouldFail(const TCHAR* PhaseWireString)
{
	if (ClaireonBPFaultInjectionInternal::BPFault_ArmedPhase.IsEmpty() || PhaseWireString == nullptr)
	{
		return false;
	}
	return ClaireonBPFaultInjectionInternal::BPFault_ArmedPhase.Equals(PhaseWireString, ESearchCase::CaseSensitive);
}

} // namespace ClaireonBPFaultInjection

#endif // WITH_CLAIREON_TESTS
