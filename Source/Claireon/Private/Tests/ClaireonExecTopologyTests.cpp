// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test knot-transparent exec joins with transient stock nodes.
// Joins count distinct origin pins, not paths or arrivals.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonExecTopology.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "UObject/Package.h"

namespace ClaireonExecTopologyTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	static UEdGraph* ExecTopo_MakeGraph()
	{
		UEdGraph* Graph = NewObject<UEdGraph>(GetTransientPackage(), NAME_None, RF_Transient);
		if (IsValid(Graph))
		{
			Graph->Schema = UEdGraphSchema_K2::StaticClass();
		}
		return Graph;
	}

	template <typename TNode>
	static TNode* ExecTopo_AddNode(UEdGraph* Graph)
	{
		TNode* Node = NewObject<TNode>(Graph, NAME_None, RF_Transient);
		Graph->Nodes.Add(Node);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	/** Direct links bypass schema type propagation, so set knot exec categories explicitly. */
	static UK2Node_Knot* ExecTopo_AddExecKnot(UEdGraph* Graph)
	{
		UK2Node_Knot* Knot = ExecTopo_AddNode<UK2Node_Knot>(Graph);
		for (UEdGraphPin* Pin : Knot->Pins)
		{
			Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
		}
		return Knot;
	}

	static UEdGraphPin* ExecTopo_FindPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const TCHAR* Name = nullptr)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != Direction)
			{
				continue;
			}
			if (Name && !Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				continue;
			}
			if (Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static void ExecTopo_Link(UEdGraphPin* From, UEdGraphPin* To)
	{
		if (From && To)
		{
			From->LinkedTo.AddUnique(To);
			To->LinkedTo.AddUnique(From);
		}
	}
}

// Case 1 -- two origins arriving directly. The baseline: no knots involved.
UNTEST_UNIT_OPTS(Claireon, ExecTopology, ExecTopology_DirectJoin, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonExecTopologyTestsNS;
	UEdGraph* Graph = ExecTopo_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_IfThenElse* Branch = ExecTopo_AddNode<UK2Node_IfThenElse>(Graph);
	UK2Node_ExecutionSequence* Target = ExecTopo_AddNode<UK2Node_ExecutionSequence>(Graph);

	ExecTopo_Link(ExecTopo_FindPin(Branch, EGPD_Output, TEXT("then")), ExecTopo_FindPin(Target, EGPD_Input));
	ExecTopo_Link(ExecTopo_FindPin(Branch, EGPD_Output, TEXT("else")), ExecTopo_FindPin(Target, EGPD_Input));

	const TArray<FClaireonExecJoin> Joins = ClaireonExecTopology::FindExecJoins(Graph);
	UNTEST_ASSERT_EQ(Joins.Num(), 1);
	UNTEST_EXPECT_EQ(Joins[0].Origins.Num(), 2);
	UNTEST_EXPECT_FALSE(Joins[0].bAnyViaKnots);
	co_return;
}

// Two origins join through a reroute.
UNTEST_UNIT_OPTS(Claireon, ExecTopology, ExecTopology_JoinThroughOneKnot, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonExecTopologyTestsNS;
	UEdGraph* Graph = ExecTopo_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_IfThenElse* Branch = ExecTopo_AddNode<UK2Node_IfThenElse>(Graph);
	UK2Node_Knot* Knot = ExecTopo_AddExecKnot(Graph);
	UK2Node_ExecutionSequence* Target = ExecTopo_AddNode<UK2Node_ExecutionSequence>(Graph);

	ExecTopo_Link(ExecTopo_FindPin(Branch, EGPD_Output, TEXT("then")), ExecTopo_FindPin(Knot, EGPD_Input));
	ExecTopo_Link(ExecTopo_FindPin(Branch, EGPD_Output, TEXT("else")), ExecTopo_FindPin(Knot, EGPD_Input));
	ExecTopo_Link(ExecTopo_FindPin(Knot, EGPD_Output), ExecTopo_FindPin(Target, EGPD_Input));

	const TArray<FClaireonExecJoin> Joins = ClaireonExecTopology::FindExecJoins(Graph);
	UNTEST_ASSERT_EQ(Joins.Num(), 1);
	UNTEST_EXPECT_EQ(Joins[0].Origins.Num(), 2);
	UNTEST_EXPECT_TRUE(Joins[0].bAnyViaKnots);
	UNTEST_EXPECT_FALSE(ClaireonExecTopology::IsKnot(Joins[0].Node));
	UNTEST_EXPECT_EQ(Joins[0].Origins[0].ViaKnots.Num(), 1);
	co_return;
}

// Case 3 -- a two-knot chain. via_knots must record the whole path, in order.
UNTEST_UNIT_OPTS(Claireon, ExecTopology, ExecTopology_JoinThroughKnotChain, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonExecTopologyTestsNS;
	UEdGraph* Graph = ExecTopo_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_IfThenElse* Branch = ExecTopo_AddNode<UK2Node_IfThenElse>(Graph);
	UK2Node_Knot* K1 = ExecTopo_AddExecKnot(Graph);
	UK2Node_Knot* K2 = ExecTopo_AddExecKnot(Graph);
	UK2Node_ExecutionSequence* Target = ExecTopo_AddNode<UK2Node_ExecutionSequence>(Graph);

	ExecTopo_Link(ExecTopo_FindPin(Branch, EGPD_Output, TEXT("then")), ExecTopo_FindPin(K1, EGPD_Input));
	ExecTopo_Link(ExecTopo_FindPin(Branch, EGPD_Output, TEXT("else")), ExecTopo_FindPin(K1, EGPD_Input));
	ExecTopo_Link(ExecTopo_FindPin(K1, EGPD_Output), ExecTopo_FindPin(K2, EGPD_Input));
	ExecTopo_Link(ExecTopo_FindPin(K2, EGPD_Output), ExecTopo_FindPin(Target, EGPD_Input));

	const TArray<FClaireonExecJoin> Joins = ClaireonExecTopology::FindExecJoins(Graph);
	UNTEST_ASSERT_EQ(Joins.Num(), 1);
	UNTEST_ASSERT_EQ(Joins[0].Origins.Num(), 2);
	UNTEST_EXPECT_EQ(Joins[0].Origins[0].ViaKnots.Num(), 2);
	co_return;
}

// A knot fan-out produces a separate join at each terminal input.
UNTEST_UNIT_OPTS(Claireon, ExecTopology, ExecTopology_BranchingKnot, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonExecTopologyTestsNS;
	UEdGraph* Graph = ExecTopo_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_IfThenElse* Branch = ExecTopo_AddNode<UK2Node_IfThenElse>(Graph);
	UK2Node_Knot* Knot = ExecTopo_AddExecKnot(Graph);
	UK2Node_ExecutionSequence* TargetA = ExecTopo_AddNode<UK2Node_ExecutionSequence>(Graph);
	UK2Node_ExecutionSequence* TargetB = ExecTopo_AddNode<UK2Node_ExecutionSequence>(Graph);

	ExecTopo_Link(ExecTopo_FindPin(Branch, EGPD_Output, TEXT("then")), ExecTopo_FindPin(Knot, EGPD_Input));
	ExecTopo_Link(ExecTopo_FindPin(Branch, EGPD_Output, TEXT("else")), ExecTopo_FindPin(Knot, EGPD_Input));
	ExecTopo_Link(ExecTopo_FindPin(Knot, EGPD_Output), ExecTopo_FindPin(TargetA, EGPD_Input));
	ExecTopo_Link(ExecTopo_FindPin(Knot, EGPD_Output), ExecTopo_FindPin(TargetB, EGPD_Input));

	const TArray<FClaireonExecJoin> Joins = ClaireonExecTopology::FindExecJoins(Graph);
	UNTEST_ASSERT_EQ(Joins.Num(), 2);
	UNTEST_EXPECT_EQ(Joins[0].Origins.Num(), 2);
	UNTEST_EXPECT_EQ(Joins[1].Origins.Num(), 2);
	co_return;
}

// A closed knot cycle must terminate without a finding.
UNTEST_UNIT_OPTS(Claireon, ExecTopology, ExecTopology_KnotCycleTerminates, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonExecTopologyTestsNS;
	UEdGraph* Graph = ExecTopo_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_IfThenElse* Branch = ExecTopo_AddNode<UK2Node_IfThenElse>(Graph);
	UK2Node_Knot* K1 = ExecTopo_AddExecKnot(Graph);
	UK2Node_Knot* K2 = ExecTopo_AddExecKnot(Graph);

	ExecTopo_Link(ExecTopo_FindPin(Branch, EGPD_Output, TEXT("then")), ExecTopo_FindPin(K1, EGPD_Input));
	// K1 -> K2 -> K1: a closed loop with no terminal non-knot input.
	ExecTopo_Link(ExecTopo_FindPin(K1, EGPD_Output), ExecTopo_FindPin(K2, EGPD_Input));
	ExecTopo_Link(ExecTopo_FindPin(K2, EGPD_Output), ExecTopo_FindPin(K1, EGPD_Input));

	const TArray<FClaireonExecJoin> Joins = ClaireonExecTopology::FindExecJoins(Graph);
	UNTEST_EXPECT_EQ(Joins.Num(), 0);
	co_return;
}

// Two routes from one origin are not a join.
UNTEST_UNIT_OPTS(Claireon, ExecTopology, ExecTopology_SingleOriginFanOutIsNotAJoin, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonExecTopologyTestsNS;
	UEdGraph* Graph = ExecTopo_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_IfThenElse* Branch = ExecTopo_AddNode<UK2Node_IfThenElse>(Graph);
	UK2Node_Knot* K1 = ExecTopo_AddExecKnot(Graph);
	UK2Node_Knot* K2 = ExecTopo_AddExecKnot(Graph);
	UK2Node_ExecutionSequence* Target = ExecTopo_AddNode<UK2Node_ExecutionSequence>(Graph);

	UEdGraphPin* Origin = ExecTopo_FindPin(Branch, EGPD_Output, TEXT("then"));
	ExecTopo_Link(Origin, ExecTopo_FindPin(K1, EGPD_Input));
	ExecTopo_Link(Origin, ExecTopo_FindPin(K2, EGPD_Input));
	ExecTopo_Link(ExecTopo_FindPin(K1, EGPD_Output), ExecTopo_FindPin(Target, EGPD_Input));
	ExecTopo_Link(ExecTopo_FindPin(K2, EGPD_Output), ExecTopo_FindPin(Target, EGPD_Input));

	const TArray<FClaireonExecJoin> Joins = ClaireonExecTopology::FindExecJoins(Graph);
	UNTEST_EXPECT_EQ(Joins.Num(), 0);
	co_return;
}

// Require stable finding order across runs.
UNTEST_UNIT_OPTS(Claireon, ExecTopology, ExecTopology_OrderIsDeterministic, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonExecTopologyTestsNS;
	UEdGraph* Graph = ExecTopo_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_IfThenElse* Branch = ExecTopo_AddNode<UK2Node_IfThenElse>(Graph);
	for (int32 Index = 0; Index < 4; ++Index)
	{
		UK2Node_ExecutionSequence* Target = ExecTopo_AddNode<UK2Node_ExecutionSequence>(Graph);
		ExecTopo_Link(ExecTopo_FindPin(Branch, EGPD_Output, TEXT("then")), ExecTopo_FindPin(Target, EGPD_Input));
		ExecTopo_Link(ExecTopo_FindPin(Branch, EGPD_Output, TEXT("else")), ExecTopo_FindPin(Target, EGPD_Input));
	}

	const TArray<FClaireonExecJoin> First = ClaireonExecTopology::FindExecJoins(Graph);
	const TArray<FClaireonExecJoin> Second = ClaireonExecTopology::FindExecJoins(Graph);
	UNTEST_ASSERT_EQ(First.Num(), 4);
	UNTEST_ASSERT_EQ(Second.Num(), First.Num());
	for (int32 Index = 0; Index < First.Num(); ++Index)
	{
		UNTEST_EXPECT_TRUE(First[Index].Node == Second[Index].Node);
		UNTEST_EXPECT_TRUE(First[Index].Pin == Second[Index].Pin);
	}
	co_return;
}

#endif // WITH_UNTESTED
