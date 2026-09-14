// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test geometry predicates on fixture-authored coordinates, including threshold boundaries.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonLintTypes.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Knot.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace ClaireonLintLayoutRuleTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	/** Use a dedicated Blueprint-owned graph so stock node allocation works without template events. */
	UEdGraph* LintLayout_MakeGraph()
	{
		static int32 Counter = 0;
		const FString AssetName = FString::Printf(TEXT("BP_LintLayoutRule_%d"), Counter++);
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
			BP, FName(TEXT("LintFixtureGraph")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (IsValid(Graph))
		{
			FBlueprintEditorUtils::AddUbergraphPage(BP, Graph);
		}
		return Graph;
	}

	template <typename TNode>
	TNode* LintLayout_AddNode(UEdGraph* Graph, int32 X, int32 Y)
	{
		TNode* Node = NewObject<TNode>(Graph, NAME_None, RF_Transient);
		Graph->Nodes.Add(Node);
		Node->CreateNewGuid();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	UK2Node_CallFunction* LintLayout_AddCall(UEdGraph* Graph, int32 X, int32 Y)
	{
		UK2Node_CallFunction* Call = LintLayout_AddNode<UK2Node_CallFunction>(Graph, X, Y);
		Call->FunctionReference.SetExternalMember(
			FName(TEXT("PrintString")), UKismetSystemLibrary::StaticClass());
		Call->ReconstructNode();
		return Call;
	}

	UK2Node_Knot* LintLayout_AddExecKnot(UEdGraph* Graph, int32 X, int32 Y)
	{
		UK2Node_Knot* Knot = LintLayout_AddNode<UK2Node_Knot>(Graph, X, Y);
		// Thresholds use the origin pin category; type the knot for fixture realism.
		for (UEdGraphPin* Pin : Knot->Pins)
		{
			Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_Exec;
		}
		return Knot;
	}

	/** Use a data source for fan-out because exec outputs support only one successor. */
	UK2Node_CallFunction* LintLayout_AddLiteralString(UEdGraph* Graph, int32 X, int32 Y)
	{
		UK2Node_CallFunction* Call = LintLayout_AddNode<UK2Node_CallFunction>(Graph, X, Y);
		Call->FunctionReference.SetExternalMember(
			FName(TEXT("MakeLiteralString")), UKismetSystemLibrary::StaticClass());
		Call->ReconstructNode();
		return Call;
	}

	UK2Node_Knot* LintLayout_AddDataKnot(UEdGraph* Graph, int32 X, int32 Y)
	{
		UK2Node_Knot* Knot = LintLayout_AddNode<UK2Node_Knot>(Graph, X, Y);
		for (UEdGraphPin* Pin : Knot->Pins)
		{
			Pin->PinType.PinCategory = UEdGraphSchema_K2::PC_String;
		}
		return Knot;
	}

	UEdGraphPin* LintLayout_FindPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const TCHAR* Name)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction && Pin->PinName.ToString() == Name)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	UEdGraphPin* LintLayout_FirstExec(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction
				&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	void LintLayout_Link(UEdGraphPin* From, UEdGraphPin* To)
	{
		if (From && To)
		{
			From->LinkedTo.AddUnique(To);
			To->LinkedTo.AddUnique(From);
		}
	}

	TArray<FClaireonLintFinding> LintLayout_Run(UEdGraph* Graph)
	{
		FClaireonLintContext Context;
		Context.Graph = Graph;
		Context.GraphName = TEXT("TestGraph");

		TArray<FClaireonLintFinding> Findings;
		ClaireonLint::RunLayoutRules(Context, Findings);
		return Findings;
	}

	int32 LintLayout_Count(const TArray<FClaireonLintFinding>& Findings, const TCHAR* Rule)
	{
		int32 Count = 0;
		for (const FClaireonLintFinding& Finding : Findings)
		{
			if (Finding.Rule == Rule)
			{
				++Count;
			}
		}
		return Count;
	}

	const FClaireonLintFinding* LintLayout_Find(const TArray<FClaireonLintFinding>& Findings, const TCHAR* Rule)
	{
		for (const FClaireonLintFinding& Finding : Findings)
		{
			if (Finding.Rule == Rule)
			{
				return &Finding;
			}
		}
		return nullptr;
	}

	/** Every finding for one rule, in emission order -- the order the chain rules fix by sorting. */
	TArray<const FClaireonLintFinding*> LintLayout_FindAll(
		const TArray<FClaireonLintFinding>& Findings, const TCHAR* Rule)
	{
		TArray<const FClaireonLintFinding*> Matches;
		for (const FClaireonLintFinding& Finding : Findings)
		{
			if (Finding.Rule == Rule)
			{
				Matches.Add(&Finding);
			}
		}
		return Matches;
	}

	/** The chain rules' consumers[] evidence array, or nullptr when absent. */
	const TArray<TSharedPtr<FJsonValue>>* LintLayout_Consumers(const FClaireonLintFinding* Finding)
	{
		const TArray<TSharedPtr<FJsonValue>>* Consumers = nullptr;
		if (Finding && Finding->Evidence.IsValid()
			&& Finding->Evidence->TryGetArrayField(TEXT("consumers"), Consumers))
		{
			return Consumers;
		}
		return nullptr;
	}

	/** One numeric evidence field, or Fallback when it is missing -- so a miss shows as a bad value, not a crash. */
	double LintLayout_Number(const FClaireonLintFinding* Finding, const TCHAR* Field, double Fallback = -1.0)
	{
		double Value = Fallback;
		if (Finding && Finding->Evidence.IsValid())
		{
			Finding->Evidence->TryGetNumberField(Field, Value);
		}
		return Value;
	}

	/** Offset both axes to give overlap fixtures nonzero width and height. */
	void LintLayout_MakePair(UEdGraph* Graph, int32 X, int32 Y, int32 Width)
	{
		UK2Node_CallFunction* A = LintLayout_AddCall(Graph, X, Y);
		UK2Node_CallFunction* B = LintLayout_AddCall(Graph, X + Width, Y + 200);
		LintLayout_Link(LintLayout_FirstExec(A, EGPD_Output), LintLayout_FirstExec(B, EGPD_Input));
	}
}

using namespace ClaireonLintLayoutRuleTestsNS;


// End-to-end span through one reroute, over the exec threshold of 1024.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, LongReroute_ExecSpanOverThreshold, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CallFunction* From = LintLayout_AddCall(Graph, 0, 0);
	UK2Node_Knot* Knot = LintLayout_AddExecKnot(Graph, 200, 0);
	UK2Node_CallFunction* To = LintLayout_AddCall(Graph, 1500, 0);

	LintLayout_Link(LintLayout_FirstExec(From, EGPD_Output), Knot->GetInputPin());
	LintLayout_Link(Knot->GetOutputPin(), LintLayout_FirstExec(To, EGPD_Input));

	const TArray<FClaireonLintFinding> Findings = LintLayout_Run(Graph);
	UNTEST_ASSERT_EQ(LintLayout_Count(Findings, TEXT("long-reroute")), 1);
	// One knot is a jog, not a chain.
	UNTEST_EXPECT_EQ(LintLayout_Count(Findings, TEXT("reroute-chain")), 0);

	const FClaireonLintFinding* Finding = LintLayout_Find(Findings, TEXT("long-reroute"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	FString PinKind;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetStringField(TEXT("pin_kind"), PinKind));
	UNTEST_EXPECT_TRUE(PinKind == TEXT("exec"));
	// Report max_span across chain consumers.
	double MaxSpan = 0.0;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("max_span"), MaxSpan));
	UNTEST_EXPECT_NEAR(MaxSpan, 1500.0, 1.0);
	co_return;
}

// Short single-knot jogs should remain silent.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, LongReroute_ShortJogSilent, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CallFunction* From = LintLayout_AddCall(Graph, 0, 0);
	UK2Node_Knot* Knot = LintLayout_AddExecKnot(Graph, 200, 100);
	UK2Node_CallFunction* To = LintLayout_AddCall(Graph, 400, 0);

	LintLayout_Link(LintLayout_FirstExec(From, EGPD_Output), Knot->GetInputPin());
	LintLayout_Link(Knot->GetOutputPin(), LintLayout_FirstExec(To, EGPD_Input));

	const TArray<FClaireonLintFinding> Findings = LintLayout_Run(Graph);
	UNTEST_EXPECT_EQ(LintLayout_Count(Findings, TEXT("long-reroute")), 0);
	UNTEST_EXPECT_EQ(LintLayout_Count(Findings, TEXT("reroute-chain")), 0);
	co_return;
}

// Two reroutes in series, reported once for the chain rather than once per hop.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, RerouteChain_TwoInSeriesReportedOnce, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CallFunction* From = LintLayout_AddCall(Graph, 0, 0);
	UK2Node_Knot* First = LintLayout_AddExecKnot(Graph, 150, 50);
	UK2Node_Knot* Second = LintLayout_AddExecKnot(Graph, 300, 50);
	UK2Node_CallFunction* To = LintLayout_AddCall(Graph, 450, 0);

	LintLayout_Link(LintLayout_FirstExec(From, EGPD_Output), First->GetInputPin());
	LintLayout_Link(First->GetOutputPin(), Second->GetInputPin());
	LintLayout_Link(Second->GetOutputPin(), LintLayout_FirstExec(To, EGPD_Input));

	const TArray<FClaireonLintFinding> Findings = LintLayout_Run(Graph);
	UNTEST_ASSERT_EQ(LintLayout_Count(Findings, TEXT("reroute-chain")), 1);
	// 450 units end to end is under the exec threshold, so the chain is the only finding.
	UNTEST_EXPECT_EQ(LintLayout_Count(Findings, TEXT("long-reroute")), 0);

	const FClaireonLintFinding* Finding = LintLayout_Find(Findings, TEXT("reroute-chain"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	double KnotCount = 0.0;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("knot_count"), KnotCount));
	UNTEST_EXPECT_NEAR(KnotCount, 2.0, 0.01);
	co_return;
}

// Aggregate findings by source pin and connected knot chain.

// One source, one knot, and three distant consumers produce one finding.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, LongReroute_FanOutReportedOncePerChain, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CallFunction* Source = LintLayout_AddLiteralString(Graph, 0, 0);
	UEdGraphPin* SourcePin = LintLayout_FindPin(Source, EGPD_Output, TEXT("ReturnValue"));
	UNTEST_ASSERT_TRUE(SourcePin != nullptr);

	UK2Node_Knot* Knot = LintLayout_AddDataKnot(Graph, 200, 0);
	LintLayout_Link(SourcePin, Knot->GetInputPin());

	// Give consumers different spans to distinguish max_span from an arbitrary span.
	for (int32 I = 0; I < 3; ++I)
	{
		UK2Node_CallFunction* Consumer = LintLayout_AddCall(Graph, 3000, I * 400);
		UEdGraphPin* ConsumerPin = LintLayout_FindPin(Consumer, EGPD_Input, TEXT("InString"));
		UNTEST_ASSERT_TRUE(ConsumerPin != nullptr);
		LintLayout_Link(Knot->GetOutputPin(), ConsumerPin);
	}

	const TArray<FClaireonLintFinding> Findings = LintLayout_Run(Graph);

	UNTEST_ASSERT_EQ(LintLayout_Count(Findings, TEXT("long-reroute")), 1);
	UNTEST_EXPECT_EQ(LintLayout_Count(Findings, TEXT("reroute-chain")), 0);

	const FClaireonLintFinding* Finding = LintLayout_Find(Findings, TEXT("long-reroute"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	UNTEST_EXPECT_EQ(Finding->RuleVersion, 2);
	UNTEST_EXPECT_NEAR(LintLayout_Number(Finding, TEXT("fan_out")), 3.0, 0.01);
	UNTEST_EXPECT_NEAR(LintLayout_Number(Finding, TEXT("knot_count")), 1.0, 0.01);
	UNTEST_EXPECT_NEAR(LintLayout_Number(Finding, TEXT("threshold")), 2048.0, 0.01);
	FString PinKind;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetStringField(TEXT("pin_kind"), PinKind));
	UNTEST_EXPECT_TRUE(PinKind == TEXT("data"));

	UNTEST_EXPECT_NEAR(LintLayout_Number(Finding, TEXT("max_span")),
		FMath::Sqrt(3000.0 * 3000.0 + 800.0 * 800.0), 1.0);

	const TArray<TSharedPtr<FJsonValue>>* Consumers = LintLayout_Consumers(Finding);
	UNTEST_ASSERT_TRUE(Consumers != nullptr);
	UNTEST_ASSERT_EQ(Consumers->Num(), 3);

	TArray<double> Spans;
	for (const TSharedPtr<FJsonValue>& Value : *Consumers)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		UNTEST_ASSERT_TRUE(Value->TryGetObject(Object));

		double Span = -1.0;
		UNTEST_EXPECT_TRUE((*Object)->TryGetNumberField(TEXT("span"), Span));
		Spans.Add(Span);

		FString ConsumerPinName;
		UNTEST_EXPECT_TRUE((*Object)->TryGetStringField(TEXT("pin"), ConsumerPinName));
		UNTEST_EXPECT_TRUE(ConsumerPinName == TEXT("InString"));

		double Reversals = -1.0;
		UNTEST_EXPECT_TRUE((*Object)->TryGetNumberField(TEXT("reversals"), Reversals));
		UNTEST_EXPECT_NEAR(Reversals, 0.0, 0.01);

		// Routed distance is at least straight-line distance; zero denotes unavailable measurement.
		double DetourRatio = 0.0;
		UNTEST_EXPECT_TRUE((*Object)->TryGetNumberField(TEXT("detour_ratio"), DetourRatio));
		UNTEST_EXPECT_TRUE(DetourRatio >= 1.0);
	}
	Spans.Sort();
	UNTEST_EXPECT_NEAR(Spans[0], 3000.0, 1.0);
	UNTEST_EXPECT_NEAR(Spans[2], FMath::Sqrt(3000.0 * 3000.0 + 800.0 * 800.0), 1.0);

	// Chain identity uses the source pin and lowest knot GUID, independent of consumer count.
	UNTEST_EXPECT_TRUE(Finding->Target
		== Source->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)
			+ TEXT(".ReturnValue->via:")
			+ Knot->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	co_return;
}

// A serial chain with one consumer differs from a distribution fan-out.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, RerouteChain_SerialChainFanOutOne, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CallFunction* Source = LintLayout_AddLiteralString(Graph, 0, 0);
	UEdGraphPin* SourcePin = LintLayout_FindPin(Source, EGPD_Output, TEXT("ReturnValue"));
	UNTEST_ASSERT_TRUE(SourcePin != nullptr);

	UK2Node_Knot* First = LintLayout_AddDataKnot(Graph, 150, 50);
	UK2Node_Knot* Second = LintLayout_AddDataKnot(Graph, 300, 50);
	UK2Node_CallFunction* Consumer = LintLayout_AddCall(Graph, 450, 0);
	UEdGraphPin* ConsumerPin = LintLayout_FindPin(Consumer, EGPD_Input, TEXT("InString"));
	UNTEST_ASSERT_TRUE(ConsumerPin != nullptr);

	LintLayout_Link(SourcePin, First->GetInputPin());
	LintLayout_Link(First->GetOutputPin(), Second->GetInputPin());
	LintLayout_Link(Second->GetOutputPin(), ConsumerPin);

	const TArray<FClaireonLintFinding> Findings = LintLayout_Run(Graph);
	UNTEST_ASSERT_EQ(LintLayout_Count(Findings, TEXT("reroute-chain")), 1);
	UNTEST_EXPECT_EQ(LintLayout_Count(Findings, TEXT("long-reroute")), 0);

	const FClaireonLintFinding* Finding = LintLayout_Find(Findings, TEXT("reroute-chain"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	UNTEST_EXPECT_EQ(Finding->RuleVersion, 2);
	UNTEST_EXPECT_NEAR(LintLayout_Number(Finding, TEXT("knot_count")), 2.0, 0.01);
	UNTEST_EXPECT_NEAR(LintLayout_Number(Finding, TEXT("fan_out")), 1.0, 0.01);

	const TArray<TSharedPtr<FJsonValue>>* Consumers = LintLayout_Consumers(Finding);
	UNTEST_ASSERT_TRUE(Consumers != nullptr);
	UNTEST_ASSERT_EQ(Consumers->Num(), 1);
	const TSharedPtr<FJsonObject>* Object = nullptr;
	UNTEST_ASSERT_TRUE((*Consumers)[0]->TryGetObject(Object));
	FString ConsumerNode;
	UNTEST_EXPECT_TRUE((*Object)->TryGetStringField(TEXT("node"), ConsumerNode));
	UNTEST_EXPECT_TRUE(ConsumerNode == Consumer->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	co_return;
}

// Disconnected knots from one source form distinct chains, sorted by lowest knot GUID.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, KnotChains_UnconnectedKnotsFromOnePinAreTwoChains, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CallFunction* Source = LintLayout_AddLiteralString(Graph, 0, 0);
	UEdGraphPin* SourcePin = LintLayout_FindPin(Source, EGPD_Output, TEXT("ReturnValue"));
	UNTEST_ASSERT_TRUE(SourcePin != nullptr);

	UK2Node_Knot* KnotA = LintLayout_AddDataKnot(Graph, 200, 0);
	UK2Node_Knot* KnotB = LintLayout_AddDataKnot(Graph, 200, 1000);
	LintLayout_Link(SourcePin, KnotA->GetInputPin());
	LintLayout_Link(SourcePin, KnotB->GetInputPin());

	UK2Node_CallFunction* ConsumerA = LintLayout_AddCall(Graph, 3000, 0);
	UK2Node_CallFunction* ConsumerB = LintLayout_AddCall(Graph, 3000, 1000);
	LintLayout_Link(KnotA->GetOutputPin(), LintLayout_FindPin(ConsumerA, EGPD_Input, TEXT("InString")));
	LintLayout_Link(KnotB->GetOutputPin(), LintLayout_FindPin(ConsumerB, EGPD_Input, TEXT("InString")));

	const TArray<FClaireonLintFinding> Findings = LintLayout_Run(Graph);
	const TArray<const FClaireonLintFinding*> Reroutes = LintLayout_FindAll(Findings, TEXT("long-reroute"));
	UNTEST_ASSERT_EQ(Reroutes.Num(), 2);

	const FString SourceId = Source->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
	const FString KnotAId = KnotA->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
	const FString KnotBId = KnotB->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
	const FString LowerId = KnotAId < KnotBId ? KnotAId : KnotBId;
	const FString HigherId = KnotAId < KnotBId ? KnotBId : KnotAId;

	UNTEST_EXPECT_TRUE(Reroutes[0]->Target == SourceId + TEXT(".ReturnValue->via:") + LowerId);
	UNTEST_EXPECT_TRUE(Reroutes[1]->Target == SourceId + TEXT(".ReturnValue->via:") + HigherId);

	for (const FClaireonLintFinding* Finding : Reroutes)
	{
		UNTEST_EXPECT_NEAR(LintLayout_Number(Finding, TEXT("knot_count")), 1.0, 0.01);
		UNTEST_EXPECT_NEAR(LintLayout_Number(Finding, TEXT("fan_out")), 1.0, 0.01);
	}
	UNTEST_EXPECT_EQ(LintLayout_Count(Findings, TEXT("reroute-chain")), 0);
	co_return;
}

// Direct links belong only to long-wire; require that finding as a positive control.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, ChainRules_DirectLinkProducesNothing, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CallFunction* Source = LintLayout_AddLiteralString(Graph, 0, 0);
	UEdGraphPin* SourcePin = LintLayout_FindPin(Source, EGPD_Output, TEXT("ReturnValue"));
	UNTEST_ASSERT_TRUE(SourcePin != nullptr);

	UK2Node_CallFunction* Consumer = LintLayout_AddCall(Graph, 5000, 0);
	UEdGraphPin* ConsumerPin = LintLayout_FindPin(Consumer, EGPD_Input, TEXT("InString"));
	UNTEST_ASSERT_TRUE(ConsumerPin != nullptr);
	LintLayout_Link(SourcePin, ConsumerPin);

	const TArray<FClaireonLintFinding> Findings = LintLayout_Run(Graph);
	UNTEST_EXPECT_EQ(LintLayout_Count(Findings, TEXT("long-reroute")), 0);
	UNTEST_EXPECT_EQ(LintLayout_Count(Findings, TEXT("reroute-chain")), 0);
	UNTEST_EXPECT_EQ(LintLayout_Count(Findings, TEXT("long-wire")), 1);
	co_return;
}

// One over-threshold consumer triggers the chain finding, which still lists every consumer.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, LongReroute_MaxSpanIsTheLargestConsumer, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CallFunction* Source = LintLayout_AddLiteralString(Graph, 0, 0);
	UEdGraphPin* SourcePin = LintLayout_FindPin(Source, EGPD_Output, TEXT("ReturnValue"));
	UNTEST_ASSERT_TRUE(SourcePin != nullptr);

	UK2Node_Knot* Knot = LintLayout_AddDataKnot(Graph, 200, 0);
	LintLayout_Link(SourcePin, Knot->GetInputPin());

	UK2Node_CallFunction* NearConsumer = LintLayout_AddCall(Graph, 500, 0);
	UK2Node_CallFunction* FarConsumer = LintLayout_AddCall(Graph, 3000, 0);
	LintLayout_Link(Knot->GetOutputPin(), LintLayout_FindPin(NearConsumer, EGPD_Input, TEXT("InString")));
	LintLayout_Link(Knot->GetOutputPin(), LintLayout_FindPin(FarConsumer, EGPD_Input, TEXT("InString")));

	const TArray<FClaireonLintFinding> Findings = LintLayout_Run(Graph);
	UNTEST_ASSERT_EQ(LintLayout_Count(Findings, TEXT("long-reroute")), 1);

	const FClaireonLintFinding* Finding = LintLayout_Find(Findings, TEXT("long-reroute"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	UNTEST_EXPECT_NEAR(LintLayout_Number(Finding, TEXT("max_span")), 3000.0, 1.0);
	UNTEST_EXPECT_NEAR(LintLayout_Number(Finding, TEXT("fan_out")), 2.0, 0.01);

	const TArray<TSharedPtr<FJsonValue>>* Consumers = LintLayout_Consumers(Finding);
	UNTEST_ASSERT_TRUE(Consumers != nullptr);
	UNTEST_ASSERT_EQ(Consumers->Num(), 2);

	bool bSawNear = false;
	bool bSawFar = false;
	for (const TSharedPtr<FJsonValue>& Value : *Consumers)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		UNTEST_ASSERT_TRUE(Value->TryGetObject(Object));
		double Span = -1.0;
		UNTEST_EXPECT_TRUE((*Object)->TryGetNumberField(TEXT("span"), Span));
		bSawNear = bSawNear || FMath::IsNearlyEqual(Span, 500.0, 1.0);
		bSawFar = bSawFar || FMath::IsNearlyEqual(Span, 3000.0, 1.0);
	}
	UNTEST_EXPECT_TRUE(bSawNear);
	UNTEST_EXPECT_TRUE(bSawFar);
	co_return;
}

// Check chain and consumer ordering with multiple chains and a fan-out.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, ChainRules_RepeatRunsAreIdentical, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	// Chain A: one knot, three consumers.
	UK2Node_CallFunction* SourceA = LintLayout_AddLiteralString(Graph, 0, 0);
	UEdGraphPin* SourceAPin = LintLayout_FindPin(SourceA, EGPD_Output, TEXT("ReturnValue"));
	UNTEST_ASSERT_TRUE(SourceAPin != nullptr);
	UK2Node_Knot* KnotA = LintLayout_AddDataKnot(Graph, 200, 0);
	LintLayout_Link(SourceAPin, KnotA->GetInputPin());
	for (int32 I = 0; I < 3; ++I)
	{
		UK2Node_CallFunction* Consumer = LintLayout_AddCall(Graph, 3000, I * 400);
		LintLayout_Link(KnotA->GetOutputPin(), LintLayout_FindPin(Consumer, EGPD_Input, TEXT("InString")));
	}

	// Chain B: two knots in series, one consumer, far enough to trip both rules.
	UK2Node_CallFunction* SourceB = LintLayout_AddLiteralString(Graph, 0, 3000);
	UEdGraphPin* SourceBPin = LintLayout_FindPin(SourceB, EGPD_Output, TEXT("ReturnValue"));
	UNTEST_ASSERT_TRUE(SourceBPin != nullptr);
	UK2Node_Knot* KnotB1 = LintLayout_AddDataKnot(Graph, 200, 3000);
	UK2Node_Knot* KnotB2 = LintLayout_AddDataKnot(Graph, 400, 3000);
	UK2Node_CallFunction* ConsumerB = LintLayout_AddCall(Graph, 3500, 3000);
	LintLayout_Link(SourceBPin, KnotB1->GetInputPin());
	LintLayout_Link(KnotB1->GetOutputPin(), KnotB2->GetInputPin());
	LintLayout_Link(KnotB2->GetOutputPin(), LintLayout_FindPin(ConsumerB, EGPD_Input, TEXT("InString")));

	const TArray<FClaireonLintFinding> First = LintLayout_Run(Graph);
	const TArray<FClaireonLintFinding> Second = LintLayout_Run(Graph);

	UNTEST_ASSERT_EQ(LintLayout_Count(First, TEXT("long-reroute")), 2);
	UNTEST_ASSERT_EQ(LintLayout_Count(First, TEXT("reroute-chain")), 1);
	UNTEST_ASSERT_EQ(First.Num(), Second.Num());

	for (int32 I = 0; I < First.Num(); ++I)
	{
		UNTEST_ASSERT_TRUE(First[I].Rule == Second[I].Rule);
		UNTEST_ASSERT_TRUE(First[I].Target == Second[I].Target);
		UNTEST_EXPECT_TRUE(First[I].Message == Second[I].Message);

		const TArray<TSharedPtr<FJsonValue>>* FirstConsumers = LintLayout_Consumers(&First[I]);
		const TArray<TSharedPtr<FJsonValue>>* SecondConsumers = LintLayout_Consumers(&Second[I]);
		if (!FirstConsumers)
		{
			UNTEST_EXPECT_TRUE(SecondConsumers == nullptr);
			continue;
		}
		UNTEST_ASSERT_TRUE(SecondConsumers != nullptr);
		UNTEST_ASSERT_EQ(FirstConsumers->Num(), SecondConsumers->Num());

		for (int32 J = 0; J < FirstConsumers->Num(); ++J)
		{
			const TSharedPtr<FJsonObject>* FirstObject = nullptr;
			const TSharedPtr<FJsonObject>* SecondObject = nullptr;
			UNTEST_ASSERT_TRUE((*FirstConsumers)[J]->TryGetObject(FirstObject));
			UNTEST_ASSERT_TRUE((*SecondConsumers)[J]->TryGetObject(SecondObject));

			FString FirstNode;
			FString SecondNode;
			UNTEST_EXPECT_TRUE((*FirstObject)->TryGetStringField(TEXT("node"), FirstNode));
			UNTEST_EXPECT_TRUE((*SecondObject)->TryGetStringField(TEXT("node"), SecondNode));
			UNTEST_EXPECT_TRUE(FirstNode == SecondNode);

			FString FirstPin;
			FString SecondPin;
			UNTEST_EXPECT_TRUE((*FirstObject)->TryGetStringField(TEXT("pin"), FirstPin));
			UNTEST_EXPECT_TRUE((*SecondObject)->TryGetStringField(TEXT("pin"), SecondPin));
			UNTEST_EXPECT_TRUE(FirstPin == SecondPin);

			double FirstSpan = -1.0;
			double SecondSpan = -2.0;
			UNTEST_EXPECT_TRUE((*FirstObject)->TryGetNumberField(TEXT("span"), FirstSpan));
			UNTEST_EXPECT_TRUE((*SecondObject)->TryGetNumberField(TEXT("span"), SecondSpan));
			UNTEST_EXPECT_NEAR(FirstSpan, SecondSpan, 0.0001);

			double FirstDetour = -1.0;
			double SecondDetour = -2.0;
			UNTEST_EXPECT_TRUE((*FirstObject)->TryGetNumberField(TEXT("detour_ratio"), FirstDetour));
			UNTEST_EXPECT_TRUE((*SecondObject)->TryGetNumberField(TEXT("detour_ratio"), SecondDetour));
			UNTEST_EXPECT_NEAR(FirstDetour, SecondDetour, 0.0001);

			double FirstReversals = -1.0;
			double SecondReversals = -2.0;
			UNTEST_EXPECT_TRUE((*FirstObject)->TryGetNumberField(TEXT("reversals"), FirstReversals));
			UNTEST_EXPECT_TRUE((*SecondObject)->TryGetNumberField(TEXT("reversals"), SecondReversals));
			UNTEST_EXPECT_NEAR(FirstReversals, SecondReversals, 0.0001);
		}
	}
	co_return;
}

// Keep long-reroute and reroute-chain predicates independent.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, ChainRules_PredicatesRemainIndependent, UNTEST_TIMEOUTMS(30000))
{
	// Short span, two knots.
	UEdGraph* ShortChain = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(ShortChain));
	{
		UK2Node_CallFunction* Source = LintLayout_AddLiteralString(ShortChain, 0, 0);
		UEdGraphPin* SourcePin = LintLayout_FindPin(Source, EGPD_Output, TEXT("ReturnValue"));
		UNTEST_ASSERT_TRUE(SourcePin != nullptr);
		UK2Node_Knot* First = LintLayout_AddDataKnot(ShortChain, 150, 50);
		UK2Node_Knot* Second = LintLayout_AddDataKnot(ShortChain, 300, 50);
		UK2Node_CallFunction* Consumer = LintLayout_AddCall(ShortChain, 450, 0);
		LintLayout_Link(SourcePin, First->GetInputPin());
		LintLayout_Link(First->GetOutputPin(), Second->GetInputPin());
		LintLayout_Link(Second->GetOutputPin(), LintLayout_FindPin(Consumer, EGPD_Input, TEXT("InString")));
	}
	const TArray<FClaireonLintFinding> ShortFindings = LintLayout_Run(ShortChain);
	UNTEST_EXPECT_EQ(LintLayout_Count(ShortFindings, TEXT("reroute-chain")), 1);
	UNTEST_EXPECT_EQ(LintLayout_Count(ShortFindings, TEXT("long-reroute")), 0);

	// Long span, one knot.
	UEdGraph* LongJog = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(LongJog));
	{
		UK2Node_CallFunction* Source = LintLayout_AddLiteralString(LongJog, 0, 0);
		UEdGraphPin* SourcePin = LintLayout_FindPin(Source, EGPD_Output, TEXT("ReturnValue"));
		UNTEST_ASSERT_TRUE(SourcePin != nullptr);
		UK2Node_Knot* Knot = LintLayout_AddDataKnot(LongJog, 200, 0);
		UK2Node_CallFunction* Consumer = LintLayout_AddCall(LongJog, 3000, 0);
		LintLayout_Link(SourcePin, Knot->GetInputPin());
		LintLayout_Link(Knot->GetOutputPin(), LintLayout_FindPin(Consumer, EGPD_Input, TEXT("InString")));
	}
	const TArray<FClaireonLintFinding> LongFindings = LintLayout_Run(LongJog);
	UNTEST_EXPECT_EQ(LintLayout_Count(LongFindings, TEXT("long-reroute")), 1);
	UNTEST_EXPECT_EQ(LintLayout_Count(LongFindings, TEXT("reroute-chain")), 0);
	co_return;
}


UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, IslandTooWide_BoundaryCases, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Wide = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Wide));
	LintLayout_MakePair(Wide, 0, 0, 13000);
	UNTEST_EXPECT_EQ(LintLayout_Count(LintLayout_Run(Wide), TEXT("island-too-wide")), 1);

	UEdGraph* Narrow = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Narrow));
	// The width threshold must allow the reference asset's widest island.
	LintLayout_MakePair(Narrow, 0, 0, 10368);
	UNTEST_EXPECT_EQ(LintLayout_Count(LintLayout_Run(Narrow), TEXT("island-too-wide")), 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, UncommentedIsland_CommentSilencesIt, UNTEST_TIMEOUTMS(30000))
{
	// Twenty connected nodes, no comment.
	UEdGraph* Bare = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Bare));
	{
		UK2Node_CallFunction* Prev = nullptr;
		for (int32 I = 0; I < 20; ++I)
		{
			UK2Node_CallFunction* Node = LintLayout_AddCall(Bare, I * 300, 0);
			if (IsValid(Prev))
			{
				LintLayout_Link(LintLayout_FirstExec(Prev, EGPD_Output), LintLayout_FirstExec(Node, EGPD_Input));
			}
			Prev = Node;
		}
	}
	UNTEST_ASSERT_EQ(LintLayout_Count(LintLayout_Run(Bare), TEXT("uncommented-island")), 1);

	// The same island with a comment box over it.
	UEdGraph* Commented = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Commented));
	{
		UK2Node_CallFunction* Prev = nullptr;
		for (int32 I = 0; I < 20; ++I)
		{
			UK2Node_CallFunction* Node = LintLayout_AddCall(Commented, I * 300, 0);
			if (IsValid(Prev))
			{
				LintLayout_Link(LintLayout_FirstExec(Prev, EGPD_Output), LintLayout_FirstExec(Node, EGPD_Input));
			}
			Prev = Node;
		}
		UEdGraphNode_Comment* Comment = LintLayout_AddNode<UEdGraphNode_Comment>(Commented, -100, -100);
		Comment->NodeWidth = 7000;
		Comment->NodeHeight = 400;
		Comment->NodeComment = TEXT("What this block is for");
	}
	UNTEST_EXPECT_EQ(LintLayout_Count(LintLayout_Run(Commented), TEXT("uncommented-island")), 0);

	// Nineteen is under the threshold.
	UEdGraph* Small = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Small));
	{
		UK2Node_CallFunction* Prev = nullptr;
		for (int32 I = 0; I < 19; ++I)
		{
			UK2Node_CallFunction* Node = LintLayout_AddCall(Small, I * 300, 0);
			if (IsValid(Prev))
			{
				LintLayout_Link(LintLayout_FirstExec(Prev, EGPD_Output), LintLayout_FirstExec(Node, EGPD_Input));
			}
			Prev = Node;
		}
	}
	UNTEST_EXPECT_EQ(LintLayout_Count(LintLayout_Run(Small), TEXT("uncommented-island")), 0);
	co_return;
}


UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, IslandColumn_DisjointAtRailSilent, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	// Without entries, both rail and island anchor fall back to minimum content X.
	LintLayout_MakePair(Graph, 0, 0, 300);
	LintLayout_MakePair(Graph, 0, 2000, 300);

	UNTEST_EXPECT_EQ(LintLayout_Count(LintLayout_Run(Graph), TEXT("island-column")), 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, IslandColumn_OverlapReportedOncePerPair, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	// Keep both islands within rail tolerance to isolate vertical overlap.
	LintLayout_MakePair(Graph, 0, 0, 600);
	LintLayout_MakePair(Graph, 300, 100, 600);

	const TArray<FClaireonLintFinding> Findings = LintLayout_Run(Graph);
	UNTEST_ASSERT_EQ(LintLayout_Count(Findings, TEXT("island-column")), 1);

	const FClaireonLintFinding* Finding = LintLayout_Find(Findings, TEXT("island-column"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	UNTEST_EXPECT_TRUE(Finding->Severity == EClaireonLintSeverity::Warning);
	FString Condition;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetStringField(TEXT("condition"), Condition));
	UNTEST_EXPECT_TRUE(Condition == TEXT("vertical_overlap"));
	FString IslandA;
	FString IslandB;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetStringField(TEXT("island_a"), IslandA));
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetStringField(TEXT("island_b"), IslandB));
	UNTEST_EXPECT_FALSE(IslandA.IsEmpty());
	UNTEST_EXPECT_FALSE(IslandB.IsEmpty());
	UNTEST_EXPECT_TRUE(Finding->Target == IslandA + TEXT("|") + IslandB);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, IslandColumn_EntryFarRightOfRailReported, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	// A singleton entry anchors the rail even though the island is ineligible for overlap checks.
	UK2Node_CustomEvent* RailAnchor = LintLayout_AddNode<UK2Node_CustomEvent>(Graph, 0, 0);
	RailAnchor->CustomFunctionName = FName(TEXT("Rail_Anchor"));

	// Place the entry outside tolerance and its feeder inside to distinguish entry X from box minimum.
	UK2Node_CustomEvent* Entry = LintLayout_AddNode<UK2Node_CustomEvent>(Graph, 1000, 2000);
	Entry->CustomFunctionName = FName(TEXT("Tested_Entry"));
	UK2Node_CallFunction* Feeder = LintLayout_AddCall(Graph, 200, 2100);
	LintLayout_Link(LintLayout_FirstExec(Entry, EGPD_Output), LintLayout_FirstExec(Feeder, EGPD_Input));

	const TArray<FClaireonLintFinding> Findings = LintLayout_Run(Graph);
	UNTEST_ASSERT_EQ(LintLayout_Count(Findings, TEXT("island-column")), 1);

	const FClaireonLintFinding* Finding = LintLayout_Find(Findings, TEXT("island-column"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	FString Condition;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetStringField(TEXT("condition"), Condition));
	UNTEST_EXPECT_TRUE(Condition == TEXT("off_rail"));
	double RailX = -1.0;
	double EntryX = -1.0;
	double Offset = -1.0;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("rail_x"), RailX));
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("island_entry_x"), EntryX));
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("offset"), Offset));
	UNTEST_EXPECT_NEAR(RailX, 0.0, 0.01);
	UNTEST_EXPECT_NEAR(EntryX, 1000.0, 0.01);
	UNTEST_EXPECT_NEAR(Offset, 1000.0, 0.01);
	FString EntrySource;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetStringField(TEXT("entry_source"), EntrySource));
	UNTEST_EXPECT_TRUE(EntrySource == TEXT("entry_node"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, IslandColumn_EntryWithinToleranceSilent, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CustomEvent* RailAnchor = LintLayout_AddNode<UK2Node_CustomEvent>(Graph, 0, 0);
	RailAnchor->CustomFunctionName = FName(TEXT("Rail_Anchor"));

	// An entry within tolerance must remain unreported despite a farther-left feeder.
	UK2Node_CustomEvent* Entry = LintLayout_AddNode<UK2Node_CustomEvent>(Graph, 600, 2000);
	Entry->CustomFunctionName = FName(TEXT("Tested_Entry"));
	UK2Node_CallFunction* Feeder = LintLayout_AddCall(Graph, -500, 2100);
	LintLayout_Link(LintLayout_FirstExec(Entry, EGPD_Output), LintLayout_FirstExec(Feeder, EGPD_Input));

	UNTEST_EXPECT_EQ(LintLayout_Count(LintLayout_Run(Graph), TEXT("island-column")), 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, IslandColumn_SingletonOverlapSilent, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	// A disconnected singleton overlapping an island is excluded from column checks.
	LintLayout_MakePair(Graph, 0, 0, 300);
	LintLayout_AddCall(Graph, 1000, 100);

	UNTEST_EXPECT_EQ(LintLayout_Count(LintLayout_Run(Graph), TEXT("island-column")), 0);
	co_return;
}

// A feeder may extend left of the rail; only right-of-rail entries are reported.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, IslandColumn_FeederLeftOfEntrySilent, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CustomEvent* Entry = LintLayout_AddNode<UK2Node_CustomEvent>(Graph, 0, 0);
	Entry->CustomFunctionName = FName(TEXT("Tested_Entry"));
	UK2Node_CallFunction* Feeder = LintLayout_AddCall(Graph, -2000, 100);
	LintLayout_Link(LintLayout_FirstExec(Entry, EGPD_Output), LintLayout_FirstExec(Feeder, EGPD_Input));

	UNTEST_EXPECT_EQ(LintLayout_Count(LintLayout_Run(Graph), TEXT("island-column")), 0);
	co_return;
}

// Exclude off-rail islands from vertical-overlap findings to avoid reporting the same placement twice.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, IslandColumn_OffRailIslandReportedOnceNotTwice, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	// With no entries, rail X is 0; the second island overlaps in Y but sits at X=5000.
	LintLayout_MakePair(Graph, 0, 0, 300);
	LintLayout_MakePair(Graph, 5000, 100, 300);

	const TArray<FClaireonLintFinding> Findings = LintLayout_Run(Graph);
	UNTEST_ASSERT_EQ(LintLayout_Count(Findings, TEXT("island-column")), 1);

	const FClaireonLintFinding* Finding = LintLayout_Find(Findings, TEXT("island-column"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	FString Condition;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetStringField(TEXT("condition"), Condition));
	UNTEST_EXPECT_TRUE(Condition == TEXT("off_rail"));
	co_return;
}

// Two in-column islands with overlapping Y ranges must still report a collision.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, IslandColumn_InColumnOverlapStillReported, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	LintLayout_MakePair(Graph, 0, 0, 300);
	LintLayout_MakePair(Graph, 0, 100, 300);

	const TArray<FClaireonLintFinding> Findings = LintLayout_Run(Graph);
	UNTEST_ASSERT_EQ(LintLayout_Count(Findings, TEXT("island-column")), 1);

	const FClaireonLintFinding* Finding = LintLayout_Find(Findings, TEXT("island-column"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	FString Condition;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetStringField(TEXT("condition"), Condition));
	UNTEST_EXPECT_TRUE(Condition == TEXT("vertical_overlap"));
	co_return;
}

// A lone off-rail event must not revive the removed per-event rule.
UNTEST_UNIT_OPTS(Claireon, LintLayoutRules, EventOffRail_RuleRetired, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintLayout_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CustomEvent* Rail = LintLayout_AddNode<UK2Node_CustomEvent>(Graph, 0, 0);
	Rail->CustomFunctionName = FName(TEXT("Rail_A"));
	UK2Node_CustomEvent* Near = LintLayout_AddNode<UK2Node_CustomEvent>(Graph, 592, 300);
	Near->CustomFunctionName = FName(TEXT("Rail_B"));
	UK2Node_CustomEvent* Far = LintLayout_AddNode<UK2Node_CustomEvent>(Graph, 2000, 600);
	Far->CustomFunctionName = FName(TEXT("Stray_C"));

	UNTEST_EXPECT_EQ(LintLayout_Count(LintLayout_Run(Graph), TEXT("event-off-rail")), 0);
	co_return;
}

#endif // WITH_UNTESTED
