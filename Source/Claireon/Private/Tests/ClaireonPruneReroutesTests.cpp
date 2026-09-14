// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Headless pruning tests for dangling ends, cascading removal, healthy chains, defaults, and isolated cycles.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonBlueprintGraphTool_PruneReroutes.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace ClaireonPruneReroutesTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	/** Use a Blueprint-owned graph because stock nodes require GetBlueprint during allocation. */
	UEdGraph* PruneReroutesTest_MakeGraph()
	{
		static int32 Counter = 0;
		const FString AssetName = FString::Printf(TEXT("BP_PruneReroutesTest_%d"), Counter++);
		UPackage* Package = CreatePackage(*(FString(TEXT("/Game/__MCPTests/")) + AssetName));
		if (!IsValid(Package))
		{
			return nullptr;
		}

		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(*AssetName), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
		if (!IsValid(BP))
		{
			return nullptr;
		}

		UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FName(TEXT("PruneReroutesFixtureGraph")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (IsValid(Graph))
		{
			FBlueprintEditorUtils::AddUbergraphPage(BP, Graph);
		}
		return Graph;
	}

	/** The fixture graph is directly outered to its Blueprint. */
	UBlueprint* PruneReroutesTest_BlueprintOf(UEdGraph* Graph)
	{
		return IsValid(Graph) ? Cast<UBlueprint>(Graph->GetOuter()) : nullptr;
	}

	template <typename TNode>
	TNode* PruneReroutesTest_AddNode(UEdGraph* Graph, int32 X, int32 Y)
	{
		TNode* Node = NewObject<TNode>(Graph, NAME_None, RF_Transient);
		Graph->Nodes.Add(Node);
		Node->CreateNewGuid();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		return Node;
	}

	/** A real node with a data OUTPUT pin, standing in for an upstream producer. */
	UK2Node_VariableGet* PruneReroutesTest_AddProducer(UEdGraph* Graph, const TCHAR* VarName, int32 X, int32 Y)
	{
		UK2Node_VariableGet* Get = PruneReroutesTest_AddNode<UK2Node_VariableGet>(Graph, X, Y);
		Get->VariableReference.SetSelfMember(FName(VarName));
		Get->PostPlacedNewNode();
		Get->AllocateDefaultPins();
		// Use a synthetic output for the undeclared variable; pruning only inspects links.
		if (Get->Pins.Num() == 0)
		{
			Get->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Int, FName(VarName));
		}
		return Get;
	}

	/** A real node with a data INPUT pin, standing in for a downstream consumer. */
	UK2Node_CallFunction* PruneReroutesTest_AddConsumer(UEdGraph* Graph, int32 X, int32 Y)
	{
		UK2Node_CallFunction* Call = PruneReroutesTest_AddNode<UK2Node_CallFunction>(Graph, X, Y);
		Call->FunctionReference.SetExternalMember(
			FName(TEXT("PrintString")), UKismetSystemLibrary::StaticClass());
		Call->PostPlacedNewNode();
		Call->AllocateDefaultPins();
		return Call;
	}

	UK2Node_Knot* PruneReroutesTest_AddKnot(UEdGraph* Graph, int32 X, int32 Y)
	{
		UK2Node_Knot* Knot = PruneReroutesTest_AddNode<UK2Node_Knot>(Graph, X, Y);
		Knot->PostPlacedNewNode();
		Knot->AllocateDefaultPins();
		return Knot;
	}

	UEdGraphPin* PruneReroutesTest_FirstDataPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction
				&& Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	void PruneReroutesTest_Link(UEdGraphPin* From, UEdGraphPin* To)
	{
		if (From && To)
		{
			From->LinkedTo.AddUnique(To);
			To->LinkedTo.AddUnique(From);
		}
	}
}

using namespace ClaireonPruneReroutesTestsNS;

// Prune an inputless knot feeding a real consumer.
UNTEST_UNIT_OPTS(Claireon, PruneReroutes, ZeroInputOneOutput_Pruned, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = PruneReroutesTest_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	UBlueprint* BP = PruneReroutesTest_BlueprintOf(Graph);
	UNTEST_ASSERT_TRUE(IsValid(BP));

	UK2Node_Knot* Knot = PruneReroutesTest_AddKnot(Graph, 0, 0);
	UK2Node_CallFunction* Consumer = PruneReroutesTest_AddConsumer(Graph, 300, 0);
	UEdGraphPin* ConsumerInput = PruneReroutesTest_FirstDataPin(Consumer, EGPD_Input);
	UNTEST_ASSERT_TRUE(ConsumerInput != nullptr);

	PruneReroutesTest_Link(Knot->GetOutputPin(), ConsumerInput);

	const ClaireonPruneReroutes::FPruneResult Result = ClaireonPruneReroutes::PruneOrphanReroutesInGraph(BP, Graph);

	UNTEST_EXPECT_EQ(Result.Total(), 1);
	UNTEST_EXPECT_EQ(Result.RemovedDanglingNoInput, 1);
	UNTEST_EXPECT_EQ(Result.RemovedOrphanComponent, 0);
	UNTEST_EXPECT_EQ(Result.RemovedDanglingNoOutput, 0);
	UNTEST_EXPECT_EQ(Result.SkippedNoInputWithDefault, 0);
	UNTEST_EXPECT_TRUE(Result.bConverged);

	UNTEST_EXPECT_FALSE(Graph->Nodes.Contains(Knot));
	UNTEST_EXPECT_EQ(ConsumerInput->LinkedTo.Num(), 0);
	co_return;
}

// Prune an outputless knot fed by a real producer.
UNTEST_UNIT_OPTS(Claireon, PruneReroutes, OneInputZeroOutputs_Pruned, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = PruneReroutesTest_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	UBlueprint* BP = PruneReroutesTest_BlueprintOf(Graph);
	UNTEST_ASSERT_TRUE(IsValid(BP));

	UK2Node_VariableGet* Producer = PruneReroutesTest_AddProducer(Graph, TEXT("SourceVar"), 0, 0);
	UK2Node_Knot* Knot = PruneReroutesTest_AddKnot(Graph, 300, 0);
	UEdGraphPin* ProducerOutput = PruneReroutesTest_FirstDataPin(Producer, EGPD_Output);
	UNTEST_ASSERT_TRUE(ProducerOutput != nullptr);

	PruneReroutesTest_Link(ProducerOutput, Knot->GetInputPin());

	const ClaireonPruneReroutes::FPruneResult Result = ClaireonPruneReroutes::PruneOrphanReroutesInGraph(BP, Graph);

	UNTEST_EXPECT_EQ(Result.Total(), 1);
	UNTEST_EXPECT_EQ(Result.RemovedDanglingNoOutput, 1);
	UNTEST_EXPECT_EQ(Result.RemovedOrphanComponent, 0);
	UNTEST_EXPECT_EQ(Result.RemovedDanglingNoInput, 0);
	UNTEST_EXPECT_EQ(Result.SkippedNoInputWithDefault, 0);
	UNTEST_EXPECT_TRUE(Result.bConverged);

	UNTEST_EXPECT_FALSE(Graph->Nodes.Contains(Knot));
	UNTEST_EXPECT_EQ(ProducerOutput->LinkedTo.Num(), 0);
	co_return;
}

// Removing each dangling tail exposes the next; prune the entire chain in one call.
UNTEST_UNIT_OPTS(Claireon, PruneReroutes, DanglingChain_CascadesInOneCall, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = PruneReroutesTest_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	UBlueprint* BP = PruneReroutesTest_BlueprintOf(Graph);
	UNTEST_ASSERT_TRUE(IsValid(BP));

	UK2Node_VariableGet* Producer = PruneReroutesTest_AddProducer(Graph, TEXT("ChainVar"), 0, 0);
	UK2Node_Knot* K1 = PruneReroutesTest_AddKnot(Graph, 300, 0);
	UK2Node_Knot* K2 = PruneReroutesTest_AddKnot(Graph, 600, 0);
	UK2Node_Knot* K3 = PruneReroutesTest_AddKnot(Graph, 900, 0);
	UEdGraphPin* ProducerOutput = PruneReroutesTest_FirstDataPin(Producer, EGPD_Output);
	UNTEST_ASSERT_TRUE(ProducerOutput != nullptr);

	PruneReroutesTest_Link(ProducerOutput, K1->GetInputPin());
	PruneReroutesTest_Link(K1->GetOutputPin(), K2->GetInputPin());
	PruneReroutesTest_Link(K2->GetOutputPin(), K3->GetInputPin());

	const ClaireonPruneReroutes::FPruneResult Result = ClaireonPruneReroutes::PruneOrphanReroutesInGraph(BP, Graph);

	UNTEST_EXPECT_EQ(Result.Total(), 3);
	UNTEST_EXPECT_EQ(Result.RemovedDanglingNoOutput, 3);
	UNTEST_EXPECT_EQ(Result.RemovedOrphanComponent, 0);
	UNTEST_EXPECT_EQ(Result.RemovedDanglingNoInput, 0);
	UNTEST_EXPECT_TRUE(Result.bConverged);
	UNTEST_EXPECT_TRUE(Result.Passes > 1);

	UNTEST_EXPECT_FALSE(Graph->Nodes.Contains(K1));
	UNTEST_EXPECT_FALSE(Graph->Nodes.Contains(K2));
	UNTEST_EXPECT_FALSE(Graph->Nodes.Contains(K3));
	UNTEST_EXPECT_EQ(ProducerOutput->LinkedTo.Num(), 0);
	co_return;
}

// Preserve healthy chains.
UNTEST_UNIT_OPTS(Claireon, PruneReroutes, HealthyChain_NothingPruned, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = PruneReroutesTest_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	UBlueprint* BP = PruneReroutesTest_BlueprintOf(Graph);
	UNTEST_ASSERT_TRUE(IsValid(BP));

	UK2Node_VariableGet* Producer = PruneReroutesTest_AddProducer(Graph, TEXT("HealthyVar"), 0, 0);
	UK2Node_Knot* K1 = PruneReroutesTest_AddKnot(Graph, 300, 0);
	UK2Node_Knot* K2 = PruneReroutesTest_AddKnot(Graph, 600, 0);
	UK2Node_CallFunction* Consumer = PruneReroutesTest_AddConsumer(Graph, 900, 0);
	UEdGraphPin* ProducerOutput = PruneReroutesTest_FirstDataPin(Producer, EGPD_Output);
	UEdGraphPin* ConsumerInput = PruneReroutesTest_FirstDataPin(Consumer, EGPD_Input);
	UNTEST_ASSERT_TRUE(ProducerOutput != nullptr);
	UNTEST_ASSERT_TRUE(ConsumerInput != nullptr);

	PruneReroutesTest_Link(ProducerOutput, K1->GetInputPin());
	PruneReroutesTest_Link(K1->GetOutputPin(), K2->GetInputPin());
	PruneReroutesTest_Link(K2->GetOutputPin(), ConsumerInput);

	const ClaireonPruneReroutes::FPruneResult Result = ClaireonPruneReroutes::PruneOrphanReroutesInGraph(BP, Graph);

	UNTEST_EXPECT_EQ(Result.Total(), 0);
	UNTEST_EXPECT_TRUE(Result.bConverged);

	UNTEST_EXPECT_TRUE(Graph->Nodes.Contains(K1));
	UNTEST_EXPECT_TRUE(Graph->Nodes.Contains(K2));
	UNTEST_ASSERT_EQ(ProducerOutput->LinkedTo.Num(), 1);
	UNTEST_EXPECT_TRUE(ProducerOutput->LinkedTo[0] == K1->GetInputPin());
	UNTEST_ASSERT_EQ(ConsumerInput->LinkedTo.Num(), 1);
	UNTEST_EXPECT_TRUE(ConsumerInput->LinkedTo[0] == K2->GetOutputPin());
	co_return;
}

// Preserve input defaults on otherwise inputless knots.
UNTEST_UNIT_OPTS(Claireon, PruneReroutes, ZeroInputWithDefault_SkippedNotPruned, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = PruneReroutesTest_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	UBlueprint* BP = PruneReroutesTest_BlueprintOf(Graph);
	UNTEST_ASSERT_TRUE(IsValid(BP));

	UK2Node_Knot* Knot = PruneReroutesTest_AddKnot(Graph, 0, 0);
	UK2Node_CallFunction* Consumer = PruneReroutesTest_AddConsumer(Graph, 300, 0);
	UEdGraphPin* ConsumerInput = PruneReroutesTest_FirstDataPin(Consumer, EGPD_Input);
	UNTEST_ASSERT_TRUE(ConsumerInput != nullptr);

	PruneReroutesTest_Link(Knot->GetOutputPin(), ConsumerInput);
	Knot->GetInputPin()->DefaultValue = TEXT("5");

	const ClaireonPruneReroutes::FPruneResult Result = ClaireonPruneReroutes::PruneOrphanReroutesInGraph(BP, Graph);

	UNTEST_EXPECT_EQ(Result.Total(), 0);
	UNTEST_EXPECT_EQ(Result.SkippedNoInputWithDefault, 1);
	UNTEST_EXPECT_TRUE(Result.bConverged);

	UNTEST_EXPECT_TRUE(Graph->Nodes.Contains(Knot));
	UNTEST_EXPECT_TRUE(Knot->GetInputPin()->DefaultValue == TEXT("5"));
	co_return;
}

// Closed knot-only cycles require component pruning because neither end is dangling.
UNTEST_UNIT_OPTS(Claireon, PruneReroutes, TwoKnotCycle_PrunedByComponentPass, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = PruneReroutesTest_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	UBlueprint* BP = PruneReroutesTest_BlueprintOf(Graph);
	UNTEST_ASSERT_TRUE(IsValid(BP));

	UK2Node_Knot* K1 = PruneReroutesTest_AddKnot(Graph, 0, 0);
	UK2Node_Knot* K2 = PruneReroutesTest_AddKnot(Graph, 300, 0);

	PruneReroutesTest_Link(K1->GetOutputPin(), K2->GetInputPin());
	PruneReroutesTest_Link(K2->GetOutputPin(), K1->GetInputPin());

	const ClaireonPruneReroutes::FPruneResult Result = ClaireonPruneReroutes::PruneOrphanReroutesInGraph(BP, Graph);

	UNTEST_EXPECT_EQ(Result.Total(), 2);
	UNTEST_EXPECT_EQ(Result.RemovedOrphanComponent, 2);
	UNTEST_EXPECT_EQ(Result.RemovedDanglingNoInput, 0);
	UNTEST_EXPECT_EQ(Result.RemovedDanglingNoOutput, 0);
	UNTEST_EXPECT_TRUE(Result.bConverged);

	UNTEST_EXPECT_FALSE(Graph->Nodes.Contains(K1));
	UNTEST_EXPECT_FALSE(Graph->Nodes.Contains(K2));
	co_return;
}

#endif // WITH_UNTESTED
