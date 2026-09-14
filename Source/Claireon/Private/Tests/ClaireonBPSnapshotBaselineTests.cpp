// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Benchmark snapshots against the frozen retired pin-connection walk.
// Synthetic fixtures exclude package I/O and asset-registry work.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonBPSnapshot.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformTime.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace ClaireonBPSnapshotBaselineTestsInternal
{

// File-local discriminator prefix: BPSnapBase_.

/** Fixture size. Large enough that the per-call cost is measurable over the loop. */
constexpr int32 BPSnapBase_NodeCount = 200;
constexpr int32 BPSnapBase_PinsPerNode = 6;
constexpr int32 BPSnapBase_Iterations = 50;

/** Chain nodes with repeated titles to exercise connectivity that title-keyed sets cannot distinguish. */
UEdGraph* BPSnapBase_BuildGraph(UObject* Outer)
{
	UEdGraph* Graph = NewObject<UEdGraph>(Outer, UEdGraph::StaticClass(), TEXT("BPSnapBase_Graph"));
	Graph->GraphGuid = FGuid::NewGuid();

	TArray<UEdGraphNode*> Nodes;
	Nodes.Reserve(BPSnapBase_NodeCount);

	for (int32 NodeIndex = 0; NodeIndex < BPSnapBase_NodeCount; ++NodeIndex)
	{
		UEdGraphNode* Node = NewObject<UEdGraphNode>(Graph);
		Node->CreateNewGuid();
		Node->NodePosX = NodeIndex * 64;
		Node->NodePosY = (NodeIndex % 8) * 64;

		for (int32 PinIndex = 0; PinIndex < BPSnapBase_PinsPerNode; ++PinIndex)
		{
			const EEdGraphPinDirection Direction =
				(PinIndex % 2 == 0) ? EGPD_Input : EGPD_Output;
			UEdGraphPin* Pin = Node->CreatePin(
				Direction,
				FName(TEXT("int")),
				FName(*FString::Printf(TEXT("Pin%d"), PinIndex)));
			Pin->DefaultValue = FString::FromInt(PinIndex);
		}

		Graph->AddNode(Node, false, false);
		Nodes.Add(Node);
	}

	for (int32 NodeIndex = 1; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		UEdGraphNode* Previous = Nodes[NodeIndex - 1];
		UEdGraphNode* Current = Nodes[NodeIndex];
		for (UEdGraphPin* OutPin : Previous->Pins)
		{
			if (OutPin->Direction != EGPD_Output)
			{
				continue;
			}
			for (UEdGraphPin* InPin : Current->Pins)
			{
				if (InPin->Direction == EGPD_Input)
				{
					OutPin->MakeLinkTo(InPin);
					break;
				}
			}
		}
	}

	return Graph;
}

/** Frozen title-keyed walk used only as a benchmark reference. */
void BPSnapBase_FrozenPreOpPinConnections(UEdGraph* Graph, TMap<FGuid, TMap<FName, TArray<FString>>>& Out)
{
	Out.Empty();
	if (!IsValid(Graph))
	{
		return;
	}
	for (UEdGraphNode* SnapNode : Graph->Nodes)
	{
		if (!IsValid(SnapNode))
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
				if (LinkedPin && IsValid(LinkedPin->GetOwningNode()))
				{
					ConnectedTo.Add(LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				}
			}
			PinConns.Add(SnapPin->PinName, ConnectedTo);
		}
		Out.Add(SnapNode->NodeGuid, PinConns);
	}
}

int32 BPSnapBase_CountFrozenLinks(const TMap<FGuid, TMap<FName, TArray<FString>>>& Map)
{
	int32 Total = 0;
	for (const TPair<FGuid, TMap<FName, TArray<FString>>>& NodePair : Map)
	{
		for (const TPair<FName, TArray<FString>>& PinPair : NodePair.Value)
		{
			Total += PinPair.Value.Num();
		}
	}
	return Total;
}

int32 BPSnapBase_CountSnapshotLinks(const FClaireonBPSnapshot& Snapshot)
{
	int32 Total = 0;
	for (const TPair<FGuid, FClaireonBPGraphTopologySnapshot>& GraphPair : Snapshot.Graphs)
	{
		for (const TPair<FGuid, FClaireonBPNodeSnapshot>& NodePair : GraphPair.Value.Nodes)
		{
			for (const TPair<FGuid, FClaireonBPPinSnapshot>& PinPair : NodePair.Value.Pins)
			{
				Total += PinPair.Value.Links.Num();
			}
		}
	}
	return Total;
}

int32 BPSnapBase_CountSnapshotPins(const FClaireonBPSnapshot& Snapshot)
{
	int32 Total = 0;
	for (const TPair<FGuid, FClaireonBPGraphTopologySnapshot>& GraphPair : Snapshot.Graphs)
	{
		for (const TPair<FGuid, FClaireonBPNodeSnapshot>& NodePair : GraphPair.Value.Nodes)
		{
			Total += NodePair.Value.Pins.Num();
		}
	}
	return Total;
}

/** Milliseconds per iteration, averaged over the loop. */
double BPSnapBase_TimeMs(TFunctionRef<void()> Body)
{
	// One untimed pass, so first-touch allocation does not land on the measurement.
	Body();

	const double Start = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < BPSnapBase_Iterations; ++Index)
	{
		Body();
	}
	const double End = FPlatformTime::Seconds();
	return ((End - Start) * 1000.0) / static_cast<double>(BPSnapBase_Iterations);
}


// Use real call nodes to measure title formatting cost, reporting cold and cached-title timings separately.

/** Fixture size for the production-node arm. */
constexpr int32 BPSnapBase_K2NodeCount = 200;

/** Chain real call nodes with two data links per edge to exercise per-link costs. */
UEdGraph* BPSnapBase_BuildK2Graph(UBlueprint*& OutBlueprint)
{
	// Use a unique name for repeated runs in one process.
	const FName FixtureName = MakeUniqueObjectName(
		GetTransientPackage(), UBlueprint::StaticClass(), TEXT("BPSnapBase_K2Fixture"));

	OutBlueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FixtureName,
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None);
	if (!IsValid(OutBlueprint) || OutBlueprint->UbergraphPages.Num() == 0)
	{
		return nullptr;
	}

	UEdGraph* Graph = OutBlueprint->UbergraphPages[0];
	if (!IsValid(Graph))
	{
		return nullptr;
	}

	TArray<UK2Node_CallFunction*> Nodes;
	Nodes.Reserve(BPSnapBase_K2NodeCount);

	for (int32 NodeIndex = 0; NodeIndex < BPSnapBase_K2NodeCount; ++NodeIndex)
	{
		UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
		Node->FunctionReference.SetExternalMember(
			FName(TEXT("Add_IntInt")), UKismetMathLibrary::StaticClass());
		Node->CreateNewGuid();
		Node->NodePosX = NodeIndex * 256;
		Node->NodePosY = (NodeIndex % 8) * 128;
		Graph->AddNode(Node, false, false);
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		Nodes.Add(Node);
	}

	// ReturnValue of node i feeds BOTH inputs of node i+1: two links per edge.
	for (int32 NodeIndex = 1; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		UEdGraphPin* OutPin = Nodes[NodeIndex - 1]->FindPin(TEXT("ReturnValue"), EGPD_Output);
		UEdGraphPin* APin = Nodes[NodeIndex]->FindPin(TEXT("A"), EGPD_Input);
		UEdGraphPin* BPin = Nodes[NodeIndex]->FindPin(TEXT("B"), EGPD_Input);
		if (OutPin && APin)
		{
			OutPin->MakeLinkTo(APin);
		}
		if (OutPin && BPin)
		{
			OutPin->MakeLinkTo(BPin);
		}
	}

	return Graph;
}

/** One timed call, no warm-up. Used for the cold-title-cache reading. */
double BPSnapBase_TimeOnceMs(TFunctionRef<void()> Body)
{
	const double Start = FPlatformTime::Seconds();
	Body();
	const double End = FPlatformTime::Seconds();
	return (End - Start) * 1000.0;
}

} // namespace ClaireonBPSnapshotBaselineTestsInternal

// Snapshot correctness and timing.

UNTEST_UNIT_OPTS(Claireon, BPSnapshotBaseline, PreOpPinConnectionsBaselineVersusValueSnapshot, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapshotBaselineTestsInternal;

	UBlueprint* Blueprint = NewObject<UBlueprint>(GetTransientPackage());
	UNTEST_ASSERT_PTR(Blueprint);

	UEdGraph* Graph = BPSnapBase_BuildGraph(Blueprint);
	UNTEST_ASSERT_PTR(Graph);
	UNTEST_ASSERT_EQ(Graph->Nodes.Num(), BPSnapBase_NodeCount);

	TArray<UEdGraph*> Graphs;
	Graphs.Add(Graph);

	TMap<FGuid, TMap<FName, TArray<FString>>> FrozenMap;
	BPSnapBase_FrozenPreOpPinConnections(Graph, FrozenMap);

	FClaireonBPSnapshot ChangedDiffSnapshot;
	UNTEST_ASSERT_TRUE(ClaireonBPSnapshot::CaptureForChangedDiff(Graphs, ChangedDiffSnapshot));

	UNTEST_EXPECT_EQ(FrozenMap.Num(), BPSnapBase_NodeCount);
	UNTEST_EXPECT_EQ(BPSnapBase_CountSnapshotPins(ChangedDiffSnapshot), BPSnapBase_NodeCount * BPSnapBase_PinsPerNode);
	UNTEST_EXPECT_EQ(BPSnapBase_CountSnapshotLinks(ChangedDiffSnapshot), BPSnapBase_CountFrozenLinks(FrozenMap));

	// A topology-only capture is never durable-effect evidence.
	UNTEST_EXPECT_TRUE(ChangedDiffSnapshot.bChangedDiffOnly);
	const FClaireonBPSnapshotDelta SelfDelta = ClaireonBPSnapshot::Diff(ChangedDiffSnapshot, ChangedDiffSnapshot);
	UNTEST_EXPECT_FALSE(SelfDelta.bComparable);
	UNTEST_EXPECT_TRUE(SelfDelta.HasDurableEffect());

	const double BaselineMs = BPSnapBase_TimeMs([Graph]()
	{
		TMap<FGuid, TMap<FName, TArray<FString>>> Scratch;
		BPSnapBase_FrozenPreOpPinConnections(Graph, Scratch);
	});

	const double ChangedDiffMs = BPSnapBase_TimeMs([&Graphs]()
	{
		FClaireonBPSnapshot Scratch;
		ClaireonBPSnapshot::CaptureForChangedDiff(Graphs, Scratch);
	});

	const double ExtractionMs = BPSnapBase_TimeMs([Blueprint, &Graphs]()
	{
		FClaireonBPSnapshot Scratch;
		ClaireonBPSnapshot::Capture(Blueprint, Graphs, EClaireonBPSnapshotFamily::Extraction, Scratch);
	});

	const double FormatMs = BPSnapBase_TimeMs([Blueprint, &Graphs]()
	{
		FClaireonBPSnapshot Scratch;
		ClaireonBPSnapshot::Capture(Blueprint, Graphs, EClaireonBPSnapshotFamily::Format, Scratch);
	});

	UE_LOG(LogTemp, Display,
		TEXT("[BPSnapshotBaseline] nodes=%d pins/node=%d links=%d iterations=%d | ")
		TEXT("PreOpPinConnections(baseline)=%.4f ms | CaptureForChangedDiff=%.4f ms | ")
		TEXT("Capture(Extraction)=%.4f ms | Capture(Format)=%.4f ms"),
		BPSnapBase_NodeCount,
		BPSnapBase_PinsPerNode,
		BPSnapBase_CountFrozenLinks(FrozenMap),
		BPSnapBase_Iterations,
		BaselineMs,
		ChangedDiffMs,
		ExtractionMs,
		FormatMs);

	// Record machine-dependent timings; assert each arm performed a usable capture.
	UNTEST_EXPECT_GT(BaselineMs, 0.0);
	UNTEST_EXPECT_GT(ChangedDiffMs, 0.0);
	UNTEST_EXPECT_GT(ExtractionMs, 0.0);
	UNTEST_EXPECT_GT(FormatMs, 0.0);

	FClaireonBPSnapshot FormatSnapshot;
	UNTEST_ASSERT_TRUE(ClaireonBPSnapshot::Capture(Blueprint, Graphs, EClaireonBPSnapshotFamily::Format, FormatSnapshot));

	UNTEST_EXPECT_EQ(FormatSnapshot.FormatProperties.NodePositions.Num(), BPSnapBase_NodeCount);

	FClaireonBPSnapshot ExtractionSnapshot;
	UNTEST_ASSERT_TRUE(ClaireonBPSnapshot::Capture(Blueprint, Graphs, EClaireonBPSnapshotFamily::Extraction, ExtractionSnapshot));
	UNTEST_EXPECT_EQ(ExtractionSnapshot.FormatProperties.NodePositions.Num(), 0);

	co_return;
}

// Measure changed-diff and full-family captures separately on production nodes.

UNTEST_UNIT_OPTS(Claireon, BPSnapshotBaseline, ProductionNodeBaselineVersusValueSnapshot, UNTEST_TIMEOUTMS(180000))
{
	using namespace ClaireonBPSnapshotBaselineTestsInternal;

	UBlueprint* Blueprint = nullptr;
	UEdGraph* Graph = BPSnapBase_BuildK2Graph(Blueprint);
	UNTEST_ASSERT_PTR(Blueprint);
	UNTEST_ASSERT_PTR(Graph);

	TArray<UEdGraph*> Graphs;
	Graphs.Add(Graph);

	// Measure the first walk before title caches are populated.
	TMap<FGuid, TMap<FName, TArray<FString>>> ColdMap;
	const double ColdBaselineMs = BPSnapBase_TimeOnceMs([Graph, &ColdMap]()
	{
		BPSnapBase_FrozenPreOpPinConnections(Graph, ColdMap);
	});

	FClaireonBPSnapshot ColdSnapshot;
	const double ColdChangedDiffMs = BPSnapBase_TimeOnceMs([&Graphs, &ColdSnapshot]()
	{
		ClaireonBPSnapshot::CaptureForChangedDiff(Graphs, ColdSnapshot);
	});
	UNTEST_ASSERT_TRUE(ColdSnapshot.bCaptured);

	const int32 FrozenLinks = BPSnapBase_CountFrozenLinks(ColdMap);
	UNTEST_EXPECT_EQ(BPSnapBase_CountSnapshotLinks(ColdSnapshot), FrozenLinks);
	UNTEST_EXPECT_GT(FrozenLinks, 0);

	// Measure cached-title steady-state cost.
	const double BaselineMs = BPSnapBase_TimeMs([Graph]()
	{
		TMap<FGuid, TMap<FName, TArray<FString>>> Scratch;
		BPSnapBase_FrozenPreOpPinConnections(Graph, Scratch);
	});

	const double ChangedDiffMs = BPSnapBase_TimeMs([&Graphs]()
	{
		FClaireonBPSnapshot Scratch;
		ClaireonBPSnapshot::CaptureForChangedDiff(Graphs, Scratch);
	});

	const double ExtractionMs = BPSnapBase_TimeMs([Blueprint, &Graphs]()
	{
		FClaireonBPSnapshot Scratch;
		ClaireonBPSnapshot::Capture(Blueprint, Graphs, EClaireonBPSnapshotFamily::Extraction, Scratch);
	});

	const double FormatMs = BPSnapBase_TimeMs([Blueprint, &Graphs]()
	{
		FClaireonBPSnapshot Scratch;
		ClaireonBPSnapshot::Capture(Blueprint, Graphs, EClaireonBPSnapshotFamily::Format, Scratch);
	});

	const int32 PinCount = BPSnapBase_CountSnapshotPins(ColdSnapshot);

	UE_LOG(LogTemp, Display,
		TEXT("[BPSnapshotBaseline][K2] nodes=%d pins=%d links=%d iterations=%d | ")
		TEXT("COLD PreOpPinConnections=%.4f ms COLD CaptureForChangedDiff=%.4f ms COLD ratio=%.3fx | ")
		TEXT("WARM PreOpPinConnections=%.4f ms WARM CaptureForChangedDiff=%.4f ms WARM ratio=%.3fx | ")
		TEXT("Capture(Extraction)=%.4f ms ratio=%.3fx | Capture(Format)=%.4f ms ratio=%.3fx"),
		Graph->Nodes.Num(), PinCount, FrozenLinks, BPSnapBase_Iterations,
		ColdBaselineMs, ColdChangedDiffMs, (ColdBaselineMs > 0.0) ? (ColdChangedDiffMs / ColdBaselineMs) : 0.0,
		BaselineMs, ChangedDiffMs, (BaselineMs > 0.0) ? (ChangedDiffMs / BaselineMs) : 0.0,
		ExtractionMs, (BaselineMs > 0.0) ? (ExtractionMs / BaselineMs) : 0.0,
		FormatMs, (BaselineMs > 0.0) ? (FormatMs / BaselineMs) : 0.0);

	UNTEST_EXPECT_GT(ColdBaselineMs, 0.0);
	UNTEST_EXPECT_GT(ColdChangedDiffMs, 0.0);
	UNTEST_EXPECT_GT(BaselineMs, 0.0);
	UNTEST_EXPECT_GT(ChangedDiffMs, 0.0);
	UNTEST_EXPECT_GT(ExtractionMs, 0.0);
	UNTEST_EXPECT_GT(FormatMs, 0.0);

	co_return;
}

#endif // WITH_UNTESTED
