// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test graph-rule classifications with stock in-memory nodes and explicit links.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonExecTopology.h"
#include "ClaireonLintTypes.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_CallFunction.h"
#include "K2Node_ExecutionSequence.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableGet.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace ClaireonLintGraphRuleTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	/**
	 * Use a dedicated graph owned by a Blueprint; stock nodes require that ownership.
	 * Avoid template events that would alter fixture counts.
	 */
	UEdGraph* LintGraphRule_MakeGraph()
	{
		static int32 Counter = 0;
		const FString AssetName = FString::Printf(TEXT("BP_LintGraphRule_%d"), Counter++);
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
	TNode* LintGraphRule_AddNode(UEdGraph* Graph, int32 X, int32 Y)
	{
		TNode* Node = NewObject<TNode>(Graph, NAME_None, RF_Transient);
		Graph->Nodes.Add(Node);
		Node->CreateNewGuid();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		return Node;
	}

	UK2Node_VariableGet* LintGraphRule_AddGet(UEdGraph* Graph, const TCHAR* VarName, int32 X, int32 Y)
	{
		UK2Node_VariableGet* Get = LintGraphRule_AddNode<UK2Node_VariableGet>(Graph, X, Y);
		Get->VariableReference.SetSelfMember(FName(VarName));
		Get->PostPlacedNewNode();
		Get->AllocateDefaultPins();
		// Use a synthetic output pin for the undeclared variable; these cases inspect geometry and links.
		if (Get->Pins.Num() == 0)
		{
			Get->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Int, FName(VarName));
		}
		return Get;
	}

	/** A pure call node (BlueprintPure UFunction), which is what pure-data-island counts. */
	UK2Node_CallFunction* LintGraphRule_AddPureCall(UEdGraph* Graph, int32 X, int32 Y)
	{
		UK2Node_CallFunction* Call = LintGraphRule_AddNode<UK2Node_CallFunction>(Graph, X, Y);
		Call->FunctionReference.SetExternalMember(
			FName(TEXT("Add_IntInt")), UKismetMathLibrary::StaticClass());
		Call->PostPlacedNewNode();
		Call->AllocateDefaultPins();
		return Call;
	}

	/** An impure call node, to stand in for the consumer at the island's edge. */
	UK2Node_CallFunction* LintGraphRule_AddImpureCall(UEdGraph* Graph, int32 X, int32 Y)
	{
		UK2Node_CallFunction* Call = LintGraphRule_AddNode<UK2Node_CallFunction>(Graph, X, Y);
		Call->FunctionReference.SetExternalMember(
			FName(TEXT("PrintString")), UKismetSystemLibrary::StaticClass());
		Call->PostPlacedNewNode();
		Call->AllocateDefaultPins();
		return Call;
	}

	UEdGraphPin* LintGraphRule_FirstPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
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

	void LintGraphRule_Link(UEdGraphPin* From, UEdGraphPin* To)
	{
		if (From && To)
		{
			From->LinkedTo.AddUnique(To);
			To->LinkedTo.AddUnique(From);
		}
	}

	/** Include computed exec topology as the production tool does. */
	TArray<FClaireonLintFinding> LintGraphRule_Run(UEdGraph* Graph)
	{
		const TArray<FClaireonExecJoin> ExecJoins = ClaireonExecTopology::FindExecJoins(Graph);

		FClaireonLintContext Context;
		Context.Graph = Graph;
		Context.GraphName = TEXT("TestGraph");
		Context.ExecJoins = &ExecJoins;

		TArray<FClaireonLintFinding> Findings;
		ClaireonLint::RunGraphRules(Context, Findings);
		return Findings;
	}

	int32 LintGraphRule_CountRule(const TArray<FClaireonLintFinding>& Findings, const TCHAR* Rule)
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

	const FClaireonLintFinding* LintGraphRule_FindRule(const TArray<FClaireonLintFinding>& Findings, const TCHAR* Rule)
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

	// Exec fixtures allocate real pins because classification depends on their direction, type, and count.

	template <typename TNode>
	TNode* LintGraphRule_AddExecNode(UEdGraph* Graph, int32 X, int32 Y)
	{
		TNode* Node = LintGraphRule_AddNode<TNode>(Graph, X, Y);
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	/** One exec pin by direction, optionally by name ("then", "else"). */
	UEdGraphPin* LintGraphRule_ExecPin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const TCHAR* Name = nullptr)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (!Pin || Pin->Direction != Direction
				|| Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			if (Name && !Pin->PinName.ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				continue;
			}
			return Pin;
		}
		return nullptr;
	}

	/** Read Sequence lanes in pin order rather than depending on generated names. */
	TArray<UEdGraphPin*> LintGraphRule_ExecPins(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		TArray<UEdGraphPin*> Pins;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction
				&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				Pins.Add(Pin);
			}
		}
		return Pins;
	}

	/** The `joins` array out of an exec-join finding's evidence, or null. */
	const TArray<TSharedPtr<FJsonValue>>* LintGraphRule_JoinRows(const FClaireonLintFinding& Finding)
	{
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (Finding.Evidence.IsValid())
		{
			Finding.Evidence->TryGetArrayField(TEXT("joins"), Rows);
		}
		return Rows;
	}

	/** One class count out of `by_class`, or -1 when the key is missing entirely. */
	int32 LintGraphRule_ByClass(const FClaireonLintFinding& Finding, const TCHAR* ClassName)
	{
		const TSharedPtr<FJsonObject>* ByClass = nullptr;
		if (!Finding.Evidence.IsValid()
			|| !Finding.Evidence->TryGetObjectField(TEXT("by_class"), ByClass)
			|| ByClass == nullptr || !ByClass->IsValid())
		{
			return -1;
		}
		int32 Count = 0;
		return (*ByClass)->TryGetNumberField(ClassName, Count) ? Count : -1;
	}

	/** Condensed JSON for one evidence row, so two runs compare element for element. */
	FString LintGraphRule_RowToJson(const TSharedPtr<FJsonValue>& Row)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		if (Row.IsValid() && Row->Type == EJson::Object && Row->AsObject().IsValid())
		{
			FJsonSerializer::Serialize(Row->AsObject().ToSharedRef(), Writer);
		}
		return Out;
	}
}

using namespace ClaireonLintGraphRuleTestsNS;


// One consumer past the 512 threshold.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, DistantGet_FarConsumerReported, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_VariableGet* Get = LintGraphRule_AddGet(Graph, TEXT("DistantVar"), 0, 0);
	UK2Node_CallFunction* Consumer = LintGraphRule_AddImpureCall(Graph, 2000, 0);
	LintGraphRule_Link(LintGraphRule_FirstPin(Get, EGPD_Output), LintGraphRule_FirstPin(Consumer, EGPD_Input));

	const TArray<FClaireonLintFinding> Findings = LintGraphRule_Run(Graph);
	UNTEST_ASSERT_EQ(LintGraphRule_CountRule(Findings, TEXT("distant-get")), 1);

	const FClaireonLintFinding* Finding = LintGraphRule_FindRule(Findings, TEXT("distant-get"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	FString Trigger;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetStringField(TEXT("trigger"), Trigger));
	UNTEST_EXPECT_TRUE(Trigger == TEXT("distance"));
	double MaxDistance = 0.0;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("max_distance"), MaxDistance));
	UNTEST_EXPECT_NEAR(MaxDistance, 2000.0, 1.0);
	co_return;
}

// Adjacent getters must not trigger distant-get.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, DistantGet_NearConsumerSilent, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_VariableGet* Get = LintGraphRule_AddGet(Graph, TEXT("NearVar"), 0, 0);
	UK2Node_CallFunction* Consumer = LintGraphRule_AddImpureCall(Graph, 256, 0);
	LintGraphRule_Link(LintGraphRule_FirstPin(Get, EGPD_Output), LintGraphRule_FirstPin(Consumer, EGPD_Input));

	UNTEST_EXPECT_EQ(LintGraphRule_CountRule(LintGraphRule_Run(Graph), TEXT("distant-get")), 0);

	// Exactly at the threshold is not "beyond" it.
	UEdGraph* AtThreshold = LintGraphRule_MakeGraph();
	UK2Node_VariableGet* Get2 = LintGraphRule_AddGet(AtThreshold, TEXT("EdgeVar"), 0, 0);
	UK2Node_CallFunction* Consumer2 = LintGraphRule_AddImpureCall(AtThreshold, 512, 0);
	LintGraphRule_Link(LintGraphRule_FirstPin(Get2, EGPD_Output), LintGraphRule_FirstPin(Consumer2, EGPD_Input));
	UNTEST_EXPECT_EQ(LintGraphRule_CountRule(LintGraphRule_Run(AtThreshold), TEXT("distant-get")), 0);
	co_return;
}

// Nearby consumers can still be far enough apart to justify separate getters.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, DistantGet_SpreadConsumersReported, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_VariableGet* Get = LintGraphRule_AddGet(Graph, TEXT("SpreadVar"), 0, 0);
	UK2Node_CallFunction* Left = LintGraphRule_AddImpureCall(Graph, -400, 0);
	UK2Node_CallFunction* Right = LintGraphRule_AddImpureCall(Graph, 400, 0);
	UEdGraphPin* Source = LintGraphRule_FirstPin(Get, EGPD_Output);
	LintGraphRule_Link(Source, LintGraphRule_FirstPin(Left, EGPD_Input));
	LintGraphRule_Link(Source, LintGraphRule_FirstPin(Right, EGPD_Input));

	const TArray<FClaireonLintFinding> Findings = LintGraphRule_Run(Graph);
	UNTEST_ASSERT_EQ(LintGraphRule_CountRule(Findings, TEXT("distant-get")), 1);

	const FClaireonLintFinding* Finding = LintGraphRule_FindRule(Findings, TEXT("distant-get"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	FString Trigger;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetStringField(TEXT("trigger"), Trigger));
	UNTEST_EXPECT_TRUE(Trigger == TEXT("spread"));
	double Spread = 0.0;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("consumer_spread"), Spread));
	UNTEST_EXPECT_NEAR(Spread, 800.0, 1.0);
	co_return;
}

// Measure through reroutes to the real consumer.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, DistantGet_KnotIsNotTheConsumer, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_VariableGet* Get = LintGraphRule_AddGet(Graph, TEXT("ReroutedVar"), 0, 0);
	UK2Node_Knot* Knot = LintGraphRule_AddNode<UK2Node_Knot>(Graph, 300, 0);
	Knot->PostPlacedNewNode();
	Knot->AllocateDefaultPins();
	UK2Node_CallFunction* Consumer = LintGraphRule_AddImpureCall(Graph, 3000, 0);

	LintGraphRule_Link(LintGraphRule_FirstPin(Get, EGPD_Output), Knot->GetInputPin());
	LintGraphRule_Link(Knot->GetOutputPin(), LintGraphRule_FirstPin(Consumer, EGPD_Input));

	const TArray<FClaireonLintFinding> Findings = LintGraphRule_Run(Graph);
	UNTEST_ASSERT_EQ(LintGraphRule_CountRule(Findings, TEXT("distant-get")), 1);

	const FClaireonLintFinding* Finding = LintGraphRule_FindRule(Findings, TEXT("distant-get"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	double MaxDistance = 0.0;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("max_distance"), MaxDistance));
	UNTEST_EXPECT_NEAR(MaxDistance, 3000.0, 1.0);
	double ConsumerCount = 0.0;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("consumer_count"), ConsumerCount));
	UNTEST_EXPECT_NEAR(ConsumerCount, 1.0, 0.01);
	co_return;
}

// Leave unused getters to orphan-nodes.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, DistantGet_UnusedGetSilent, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	LintGraphRule_AddGet(Graph, TEXT("OrphanVar"), 0, 0);

	UNTEST_EXPECT_EQ(LintGraphRule_CountRule(LintGraphRule_Run(Graph), TEXT("distant-get")), 0);
	co_return;
}


UNTEST_UNIT_OPTS(Claireon, LintGraphRules, PureIsland_SixPureOneConsumerReported, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	// Six pure nodes chained, feeding one impure consumer.
	TArray<UK2Node_CallFunction*> Pure;
	for (int32 I = 0; I < 6; ++I)
	{
		Pure.Add(LintGraphRule_AddPureCall(Graph, I * 200, 0));
	}
	for (int32 I = 0; I + 1 < Pure.Num(); ++I)
	{
		LintGraphRule_Link(
			LintGraphRule_FirstPin(Pure[I], EGPD_Output),
			LintGraphRule_FirstPin(Pure[I + 1], EGPD_Input));
	}
	UK2Node_CallFunction* Consumer = LintGraphRule_AddImpureCall(Graph, 1400, 0);
	LintGraphRule_Link(
		LintGraphRule_FirstPin(Pure.Last(), EGPD_Output),
		LintGraphRule_FirstPin(Consumer, EGPD_Input));

	const TArray<FClaireonLintFinding> Findings = LintGraphRule_Run(Graph);
	UNTEST_ASSERT_EQ(LintGraphRule_CountRule(Findings, TEXT("pure-data-island")), 1);

	const FClaireonLintFinding* Finding = LintGraphRule_FindRule(Findings, TEXT("pure-data-island"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	double PureCount = 0.0;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("pure_node_count"), PureCount));
	UNTEST_EXPECT_NEAR(PureCount, 6.0, 0.01);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LintGraphRules, PureIsland_BelowThresholdSilent, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	TArray<UK2Node_CallFunction*> Pure;
	for (int32 I = 0; I < 5; ++I)
	{
		Pure.Add(LintGraphRule_AddPureCall(Graph, I * 200, 0));
	}
	for (int32 I = 0; I + 1 < Pure.Num(); ++I)
	{
		LintGraphRule_Link(
			LintGraphRule_FirstPin(Pure[I], EGPD_Output),
			LintGraphRule_FirstPin(Pure[I + 1], EGPD_Input));
	}
	UK2Node_CallFunction* Consumer = LintGraphRule_AddImpureCall(Graph, 1200, 0);
	LintGraphRule_Link(
		LintGraphRule_FirstPin(Pure.Last(), EGPD_Output),
		LintGraphRule_FirstPin(Consumer, EGPD_Input));

	UNTEST_EXPECT_EQ(LintGraphRule_CountRule(LintGraphRule_Run(Graph), TEXT("pure-data-island")), 0);
	co_return;
}

// Clusters with multiple consumers must not be recommended for extraction.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, PureIsland_TwoConsumersSilent, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	TArray<UK2Node_CallFunction*> Pure;
	for (int32 I = 0; I < 6; ++I)
	{
		Pure.Add(LintGraphRule_AddPureCall(Graph, I * 200, 0));
	}
	for (int32 I = 0; I + 1 < Pure.Num(); ++I)
	{
		LintGraphRule_Link(
			LintGraphRule_FirstPin(Pure[I], EGPD_Output),
			LintGraphRule_FirstPin(Pure[I + 1], EGPD_Input));
	}
	UEdGraphPin* Tail = LintGraphRule_FirstPin(Pure.Last(), EGPD_Output);
	UK2Node_CallFunction* ConsumerA = LintGraphRule_AddImpureCall(Graph, 1400, 0);
	UK2Node_CallFunction* ConsumerB = LintGraphRule_AddImpureCall(Graph, 1400, 400);
	LintGraphRule_Link(Tail, LintGraphRule_FirstPin(ConsumerA, EGPD_Input));
	LintGraphRule_Link(Tail, LintGraphRule_FirstPin(ConsumerB, EGPD_Input));

	UNTEST_EXPECT_EQ(LintGraphRule_CountRule(LintGraphRule_Run(Graph), TEXT("pure-data-island")), 0);
	co_return;
}

// Traverse reroutes without counting them toward the island threshold.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, PureIsland_KnotsDoNotInflateCount, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	TArray<UK2Node_CallFunction*> Pure;
	for (int32 I = 0; I < 5; ++I)
	{
		Pure.Add(LintGraphRule_AddPureCall(Graph, I * 300, 0));
	}
	// Chain them through a knot each, so the component holds 5 pure + 4 knots.
	for (int32 I = 0; I + 1 < Pure.Num(); ++I)
	{
		UK2Node_Knot* Knot = LintGraphRule_AddNode<UK2Node_Knot>(Graph, I * 300 + 150, 0);
		Knot->PostPlacedNewNode();
		Knot->AllocateDefaultPins();
		LintGraphRule_Link(LintGraphRule_FirstPin(Pure[I], EGPD_Output), Knot->GetInputPin());
		LintGraphRule_Link(Knot->GetOutputPin(), LintGraphRule_FirstPin(Pure[I + 1], EGPD_Input));
	}
	UK2Node_CallFunction* Consumer = LintGraphRule_AddImpureCall(Graph, 1600, 0);
	LintGraphRule_Link(
		LintGraphRule_FirstPin(Pure.Last(), EGPD_Output),
		LintGraphRule_FirstPin(Consumer, EGPD_Input));

	UNTEST_EXPECT_EQ(LintGraphRule_CountRule(LintGraphRule_Run(Graph), TEXT("pure-data-island")), 0);
	co_return;
}

// Pure-island traversal relies on pure nodes having no exec pins and connected knots carrying data types.

UNTEST_UNIT_OPTS(Claireon, LintGraphRules, PureIsland_TraversableNodesHaveNoExecPin,
	UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CallFunction* Pure = LintGraphRule_AddPureCall(Graph, 0, 0);
	UK2Node_CallFunction* Impure = LintGraphRule_AddImpureCall(Graph, 400, 0);
	UNTEST_ASSERT_TRUE(IsValid(Pure));
	UNTEST_ASSERT_TRUE(IsValid(Impure));

	UNTEST_ASSERT_TRUE(ClaireonExecTopology::IsPureNonKnot(Pure));
	UNTEST_ASSERT_FALSE(ClaireonExecTopology::IsPureNonKnot(Impure));

	int32 PureExecPins = 0;
	for (const UEdGraphPin* Pin : Pure->Pins)
	{
		if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			++PureExecPins;
		}
	}
	UNTEST_EXPECT_EQ(PureExecPins, 0);

	// Use an impure node as the control for exec-pin allocation.
	int32 ImpureExecPins = 0;
	for (const UEdGraphPin* Pin : Impure->Pins)
	{
		if (Pin && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			++ImpureExecPins;
		}
	}
	UNTEST_EXPECT_TRUE(ImpureExecPins > 0);
	co_return;
}

// An impure consumer remains outside the pure island.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, PureIsland_ImpureConsumerIsNotAMember,
	UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	TArray<UK2Node_CallFunction*> Pure;
	for (int32 I = 0; I < 6; ++I)
	{
		Pure.Add(LintGraphRule_AddPureCall(Graph, I * 200, 0));
	}
	for (int32 I = 0; I + 1 < Pure.Num(); ++I)
	{
		LintGraphRule_Link(
			LintGraphRule_FirstPin(Pure[I], EGPD_Output),
			LintGraphRule_FirstPin(Pure[I + 1], EGPD_Input));
	}

	UK2Node_CallFunction* Consumer = LintGraphRule_AddImpureCall(Graph, 1400, 0);
	UK2Node_CallFunction* Downstream = LintGraphRule_AddImpureCall(Graph, 1800, 0);
	LintGraphRule_Link(
		LintGraphRule_FirstPin(Pure.Last(), EGPD_Output),
		LintGraphRule_FirstPin(Consumer, EGPD_Input));

	auto FirstExecPin = [](UEdGraphNode* Node, EEdGraphPinDirection Dir) -> UEdGraphPin*
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Dir
				&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}
		return nullptr;
	};
	LintGraphRule_Link(FirstExecPin(Consumer, EGPD_Output), FirstExecPin(Downstream, EGPD_Input));

	const TArray<FClaireonLintFinding> Findings = LintGraphRule_Run(Graph);
	const FClaireonLintFinding* Finding = LintGraphRule_FindRule(Findings, TEXT("pure-data-island"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);

	double PureCount = 0.0;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("pure_node_count"), PureCount));
	UNTEST_EXPECT_NEAR(PureCount, 6.0, 0.01);

	const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
	UNTEST_ASSERT_TRUE(Finding->Evidence->TryGetArrayField(TEXT("members"), Members));
	UNTEST_ASSERT_TRUE(Members != nullptr);
	TArray<FString> MemberIds;
	for (const TSharedPtr<FJsonValue>& Value : *Members)
	{
		FString Id;
		if (Value.IsValid() && Value->TryGetString(Id))
		{
			MemberIds.Add(Id);
		}
	}
	UNTEST_EXPECT_EQ(MemberIds.Num(), 6);
	UNTEST_EXPECT_FALSE(
		MemberIds.Contains(Consumer->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)));
	UNTEST_EXPECT_FALSE(
		MemberIds.Contains(Downstream->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)));
	co_return;
}

// Two pure islands connected only through their consumers' exec chain must remain separate.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, PureIsland_ExecEdgeDoesNotMergeTwoIslands,
	UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	auto BuildCluster = [Graph](int32 BaseY) -> UK2Node_CallFunction*
	{
		TArray<UK2Node_CallFunction*> Pure;
		for (int32 I = 0; I < 6; ++I)
		{
			Pure.Add(LintGraphRule_AddPureCall(Graph, I * 200, BaseY));
		}
		for (int32 I = 0; I + 1 < Pure.Num(); ++I)
		{
			LintGraphRule_Link(
				LintGraphRule_FirstPin(Pure[I], EGPD_Output),
				LintGraphRule_FirstPin(Pure[I + 1], EGPD_Input));
		}
		UK2Node_CallFunction* Consumer = LintGraphRule_AddImpureCall(Graph, 1400, BaseY);
		LintGraphRule_Link(
			LintGraphRule_FirstPin(Pure.Last(), EGPD_Output),
			LintGraphRule_FirstPin(Consumer, EGPD_Input));
		return Consumer;
	};

	UK2Node_CallFunction* ConsumerA = BuildCluster(0);
	UK2Node_CallFunction* ConsumerB = BuildCluster(600);
	UNTEST_ASSERT_TRUE(IsValid(ConsumerA));
	UNTEST_ASSERT_TRUE(IsValid(ConsumerB));

	auto FirstExecPin = [](UEdGraphNode* Node, EEdGraphPinDirection Dir) -> UEdGraphPin*
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Dir
				&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}
		return nullptr;
	};

	UEdGraphPin* BridgeOut = FirstExecPin(ConsumerA, EGPD_Output);
	UEdGraphPin* BridgeIn = FirstExecPin(ConsumerB, EGPD_Input);
	UNTEST_ASSERT_TRUE(BridgeOut != nullptr);
	UNTEST_ASSERT_TRUE(BridgeIn != nullptr);
	LintGraphRule_Link(BridgeOut, BridgeIn);

	const TArray<FClaireonLintFinding> Findings = LintGraphRule_Run(Graph);

	UNTEST_EXPECT_EQ(LintGraphRule_CountRule(Findings, TEXT("pure-data-island")), 2);

	for (const FClaireonLintFinding& Finding : Findings)
	{
		if (Finding.Rule != TEXT("pure-data-island"))
		{
			continue;
		}
		double PureCount = 0.0;
		UNTEST_EXPECT_TRUE(Finding.Evidence->TryGetNumberField(TEXT("pure_node_count"), PureCount));
		UNTEST_EXPECT_NEAR(PureCount, 6.0, 0.01);
	}
	co_return;
}

// Test join dispositions: exempt convergence remains visible, while Sequence lanes are not exclusive branches.

// Include a real exec edge in the no-join control.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, ExecJoin_NoJoinsSilent, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CallFunction* First = LintGraphRule_AddImpureCall(Graph, 0, 0);
	UK2Node_CallFunction* Second = LintGraphRule_AddImpureCall(Graph, 400, 0);
	UEdGraphPin* FromExec = LintGraphRule_ExecPin(First, EGPD_Output);
	UEdGraphPin* ToExec = LintGraphRule_ExecPin(Second, EGPD_Input);
	UNTEST_ASSERT_TRUE(FromExec != nullptr && ToExec != nullptr);
	LintGraphRule_Link(FromExec, ToExec);
	UNTEST_ASSERT_EQ(FromExec->LinkedTo.Num(), 1);

	UNTEST_ASSERT_EQ(ClaireonExecTopology::FindExecJoins(Graph).Num(), 0);
	UNTEST_EXPECT_EQ(LintGraphRule_CountRule(LintGraphRule_Run(Graph), TEXT("exec-join")), 0);
	co_return;
}

// Tail closure size distinguishes terminal-single-call-tail from shared-tail.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, ExecJoin_OrdinaryTailIsOneGraphFinding, UNTEST_TIMEOUTMS(30000))
{
	// One-node terminal tail.
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CallFunction* OriginA = LintGraphRule_AddImpureCall(Graph, 0, 0);
	UK2Node_CallFunction* OriginB = LintGraphRule_AddImpureCall(Graph, 0, 400);
	UK2Node_CallFunction* Tail = LintGraphRule_AddImpureCall(Graph, 400, 200);
	UEdGraphPin* TailIn = LintGraphRule_ExecPin(Tail, EGPD_Input);
	UNTEST_ASSERT_TRUE(TailIn != nullptr);
	LintGraphRule_Link(LintGraphRule_ExecPin(OriginA, EGPD_Output), TailIn);
	LintGraphRule_Link(LintGraphRule_ExecPin(OriginB, EGPD_Output), TailIn);
	UNTEST_ASSERT_EQ(TailIn->LinkedTo.Num(), 2);

	const TArray<FClaireonLintFinding> Findings = LintGraphRule_Run(Graph);
	UNTEST_ASSERT_EQ(LintGraphRule_CountRule(Findings, TEXT("exec-join")), 1);

	const FClaireonLintFinding* Finding = LintGraphRule_FindRule(Findings, TEXT("exec-join"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	UNTEST_EXPECT_EQ(Finding->RuleVersion, 2);
	UNTEST_EXPECT_TRUE(Finding->Severity == EClaireonLintSeverity::Info);
	UNTEST_EXPECT_TRUE(Finding->Confidence == EClaireonLintConfidence::High);
	UNTEST_EXPECT_FALSE(Finding->SuggestedFix.IsValid());

	int32 JoinCount = 0;
	int32 ReportableCount = 0;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("join_count"), JoinCount));
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("reportable_count"), ReportableCount));
	UNTEST_EXPECT_EQ(JoinCount, 1);
	UNTEST_EXPECT_EQ(ReportableCount, 1);

	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*Finding, TEXT("terminal-single-call-tail")), 1);
	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*Finding, TEXT("function-result")), 0);
	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*Finding, TEXT("all-branches")), 0);
	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*Finding, TEXT("control-pin")), 0);
	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*Finding, TEXT("shared-tail")), 0);

	const FString ExpectedTarget = Tail->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)
		+ TEXT(".") + TailIn->PinName.ToString();
	UNTEST_EXPECT_TRUE(Finding->Target == ExpectedTarget);

	const TArray<TSharedPtr<FJsonValue>>* Rows = LintGraphRule_JoinRows(*Finding);
	UNTEST_ASSERT_TRUE(Rows != nullptr);
	UNTEST_ASSERT_EQ(Rows->Num(), 1);
	const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
	UNTEST_ASSERT_TRUE(Row.IsValid());

	FString ClassName;
	FString Disposition;
	int32 OriginCount = 0;
	int32 ClosureSize = 0;
	int32 ForeignEntries = 0;
	UNTEST_EXPECT_TRUE(Row->TryGetStringField(TEXT("class"), ClassName));
	UNTEST_EXPECT_TRUE(ClassName == TEXT("terminal-single-call-tail"));
	UNTEST_EXPECT_TRUE(Row->TryGetStringField(TEXT("disposition"), Disposition));
	UNTEST_EXPECT_TRUE(Disposition == TEXT("leave"));
	UNTEST_EXPECT_TRUE(Row->TryGetNumberField(TEXT("origin_count"), OriginCount));
	UNTEST_EXPECT_EQ(OriginCount, 2);
	UNTEST_EXPECT_TRUE(Row->TryGetNumberField(TEXT("closure_size"), ClosureSize));
	UNTEST_EXPECT_EQ(ClosureSize, 1);
	UNTEST_EXPECT_TRUE(Row->TryGetNumberField(TEXT("foreign_exec_entries"), ForeignEntries));
	UNTEST_EXPECT_EQ(ForeignEntries, 0);

	FString RowTarget;
	UNTEST_EXPECT_TRUE(Row->TryGetStringField(TEXT("target"), RowTarget));
	UNTEST_EXPECT_TRUE(RowTarget == ExpectedTarget);

	// Adding a second tail node changes the disposition to extract.
	UEdGraph* Continuing = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Continuing));
	UK2Node_CallFunction* COriginA = LintGraphRule_AddImpureCall(Continuing, 0, 0);
	UK2Node_CallFunction* COriginB = LintGraphRule_AddImpureCall(Continuing, 0, 400);
	UK2Node_CallFunction* CTail = LintGraphRule_AddImpureCall(Continuing, 400, 200);
	UK2Node_CallFunction* CAfter = LintGraphRule_AddImpureCall(Continuing, 800, 200);
	UEdGraphPin* CTailIn = LintGraphRule_ExecPin(CTail, EGPD_Input);
	LintGraphRule_Link(LintGraphRule_ExecPin(COriginA, EGPD_Output), CTailIn);
	LintGraphRule_Link(LintGraphRule_ExecPin(COriginB, EGPD_Output), CTailIn);
	LintGraphRule_Link(LintGraphRule_ExecPin(CTail, EGPD_Output), LintGraphRule_ExecPin(CAfter, EGPD_Input));

	const TArray<FClaireonLintFinding> CFindings = LintGraphRule_Run(Continuing);
	UNTEST_ASSERT_EQ(LintGraphRule_CountRule(CFindings, TEXT("exec-join")), 1);
	const FClaireonLintFinding* CFinding = LintGraphRule_FindRule(CFindings, TEXT("exec-join"));
	UNTEST_ASSERT_TRUE(CFinding != nullptr);
	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*CFinding, TEXT("shared-tail")), 1);
	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*CFinding, TEXT("terminal-single-call-tail")), 0);
	UNTEST_EXPECT_TRUE(CFinding->Severity == EClaireonLintSeverity::Info);

	const TArray<TSharedPtr<FJsonValue>>* CRows = LintGraphRule_JoinRows(*CFinding);
	UNTEST_ASSERT_TRUE(CRows != nullptr);
	UNTEST_ASSERT_EQ(CRows->Num(), 1);
	const TSharedPtr<FJsonObject> CRow = (*CRows)[0]->AsObject();
	UNTEST_ASSERT_TRUE(CRow.IsValid());
	FString CDisposition;
	int32 CClosureSize = 0;
	UNTEST_EXPECT_TRUE(CRow->TryGetStringField(TEXT("disposition"), CDisposition));
	UNTEST_EXPECT_TRUE(CDisposition == TEXT("extract"));
	UNTEST_EXPECT_TRUE(CRow->TryGetNumberField(TEXT("closure_size"), CClosureSize));
	UNTEST_EXPECT_EQ(CClosureSize, 2);
	co_return;
}

// Return convergence appears in evidence but is not reportable.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, ExecJoin_FunctionResultIsIgnoredNotDropped, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_CallFunction* ExitA = LintGraphRule_AddImpureCall(Graph, 0, 0);
	UK2Node_CallFunction* ExitB = LintGraphRule_AddImpureCall(Graph, 0, 400);
	UK2Node_FunctionResult* Result = LintGraphRule_AddExecNode<UK2Node_FunctionResult>(Graph, 400, 200);
	UNTEST_ASSERT_TRUE(IsValid(Result));
	UEdGraphPin* ResultIn = LintGraphRule_ExecPin(Result, EGPD_Input);
	UNTEST_ASSERT_TRUE(ResultIn != nullptr);
	LintGraphRule_Link(LintGraphRule_ExecPin(ExitA, EGPD_Output), ResultIn);
	LintGraphRule_Link(LintGraphRule_ExecPin(ExitB, EGPD_Output), ResultIn);

	const TArray<FClaireonLintFinding> Findings = LintGraphRule_Run(Graph);
	UNTEST_ASSERT_EQ(LintGraphRule_CountRule(Findings, TEXT("exec-join")), 1);

	const FClaireonLintFinding* Finding = LintGraphRule_FindRule(Findings, TEXT("exec-join"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*Finding, TEXT("function-result")), 1);

	int32 JoinCount = 0;
	int32 ReportableCount = 0;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("join_count"), JoinCount));
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("reportable_count"), ReportableCount));
	UNTEST_EXPECT_EQ(JoinCount, 1);
	UNTEST_EXPECT_EQ(ReportableCount, 0);

	UNTEST_EXPECT_TRUE(Finding->Severity == EClaireonLintSeverity::Info);
	const FString ExpectedTarget = Result->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)
		+ TEXT(".") + ResultIn->PinName.ToString();
	UNTEST_EXPECT_TRUE(Finding->Target == ExpectedTarget);

	const TArray<TSharedPtr<FJsonValue>>* Rows = LintGraphRule_JoinRows(*Finding);
	UNTEST_ASSERT_TRUE(Rows != nullptr);
	UNTEST_ASSERT_EQ(Rows->Num(), 1);
	const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
	UNTEST_ASSERT_TRUE(Row.IsValid());
	FString ClassName;
	FString Disposition;
	UNTEST_EXPECT_TRUE(Row->TryGetStringField(TEXT("class"), ClassName));
	UNTEST_EXPECT_TRUE(ClassName == TEXT("function-result"));
	UNTEST_EXPECT_TRUE(Row->TryGetStringField(TEXT("disposition"), Disposition));
	UNTEST_EXPECT_TRUE(Disposition == TEXT("ignore"));
	co_return;
}

// Both Branch outputs reaching the same call produce an all-branches warning.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, ExecJoin_BranchAllPathsIsWarning, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_IfThenElse* Branch = LintGraphRule_AddExecNode<UK2Node_IfThenElse>(Graph, 0, 0);
	UK2Node_CallFunction* Shared = LintGraphRule_AddImpureCall(Graph, 400, 0);
	UEdGraphPin* Then = LintGraphRule_ExecPin(Branch, EGPD_Output, TEXT("then"));
	UEdGraphPin* Else = LintGraphRule_ExecPin(Branch, EGPD_Output, TEXT("else"));
	UEdGraphPin* SharedIn = LintGraphRule_ExecPin(Shared, EGPD_Input);
	UNTEST_ASSERT_TRUE(Then != nullptr && Else != nullptr && SharedIn != nullptr);
	LintGraphRule_Link(Then, SharedIn);
	LintGraphRule_Link(Else, SharedIn);

	const TArray<FClaireonLintFinding> Findings = LintGraphRule_Run(Graph);
	UNTEST_ASSERT_EQ(LintGraphRule_CountRule(Findings, TEXT("exec-join")), 1);

	const FClaireonLintFinding* Finding = LintGraphRule_FindRule(Findings, TEXT("exec-join"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);
	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*Finding, TEXT("all-branches")), 1);
	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*Finding, TEXT("terminal-single-call-tail")), 0);

	UNTEST_EXPECT_TRUE(Finding->Severity == EClaireonLintSeverity::Warning);
	int32 ReportableCount = 0;
	UNTEST_EXPECT_TRUE(Finding->Evidence->TryGetNumberField(TEXT("reportable_count"), ReportableCount));
	UNTEST_EXPECT_EQ(ReportableCount, 1);

	// Native Branch exclusivity is known rather than assumed.
	UNTEST_EXPECT_TRUE(Finding->Confidence == EClaireonLintConfidence::High);

	UNTEST_EXPECT_FALSE(Finding->SuggestedFix.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* Rows = LintGraphRule_JoinRows(*Finding);
	UNTEST_ASSERT_TRUE(Rows != nullptr);
	UNTEST_ASSERT_EQ(Rows->Num(), 1);
	const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
	UNTEST_ASSERT_TRUE(Row.IsValid());

	FString ClassName;
	FString Disposition;
	FString BranchNode;
	FString Exclusivity;
	FString UnusedNote;
	int32 CandidateCount = 0;
	UNTEST_EXPECT_TRUE(Row->TryGetStringField(TEXT("class"), ClassName));
	UNTEST_EXPECT_TRUE(ClassName == TEXT("all-branches"));
	UNTEST_EXPECT_TRUE(Row->TryGetStringField(TEXT("disposition"), Disposition));
	UNTEST_EXPECT_TRUE(Disposition == TEXT("offer"));

	UNTEST_EXPECT_TRUE(Row->TryGetStringField(TEXT("branch_node"), BranchNode));
	UNTEST_EXPECT_TRUE(BranchNode == Branch->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	UNTEST_EXPECT_TRUE(Row->TryGetStringField(TEXT("branch_exclusivity"), Exclusivity));
	UNTEST_EXPECT_TRUE(Exclusivity == TEXT("known-exclusive"));
	UNTEST_EXPECT_TRUE(Row->TryGetNumberField(TEXT("branch_candidate_count"), CandidateCount));
	UNTEST_EXPECT_EQ(CandidateCount, 1);

	int32 BranchDistance = 0;
	UNTEST_EXPECT_TRUE(Row->TryGetNumberField(TEXT("branch_exec_distance"), BranchDistance));
	UNTEST_EXPECT_EQ(BranchDistance, 1);

	UNTEST_EXPECT_FALSE(Row->TryGetStringField(TEXT("branch_exclusivity_note"), UnusedNote));
	co_return;
}

// Sequence lanes both execute; hoisting their common call would change invocation count.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, ExecJoin_SequenceLanesAreNotAllBranches, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_ExecutionSequence* Sequence = LintGraphRule_AddExecNode<UK2Node_ExecutionSequence>(Graph, 0, 0);
	UK2Node_CallFunction* Shared = LintGraphRule_AddImpureCall(Graph, 400, 0);
	const TArray<UEdGraphPin*> Lanes = LintGraphRule_ExecPins(Sequence, EGPD_Output);
	UEdGraphPin* SharedIn = LintGraphRule_ExecPin(Shared, EGPD_Input);
	UNTEST_ASSERT_EQ(Lanes.Num(), 2);
	UNTEST_ASSERT_TRUE(SharedIn != nullptr);
	LintGraphRule_Link(Lanes[0], SharedIn);
	LintGraphRule_Link(Lanes[1], SharedIn);

	const TArray<FClaireonLintFinding> Findings = LintGraphRule_Run(Graph);
	UNTEST_ASSERT_EQ(LintGraphRule_CountRule(Findings, TEXT("exec-join")), 1);

	const FClaireonLintFinding* Finding = LintGraphRule_FindRule(Findings, TEXT("exec-join"));
	UNTEST_ASSERT_TRUE(Finding != nullptr);

	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*Finding, TEXT("all-branches")), 0);
	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*Finding, TEXT("terminal-single-call-tail")), 1);
	UNTEST_EXPECT_TRUE(Finding->Severity == EClaireonLintSeverity::Info);

	const TArray<TSharedPtr<FJsonValue>>* Rows = LintGraphRule_JoinRows(*Finding);
	UNTEST_ASSERT_TRUE(Rows != nullptr);
	UNTEST_ASSERT_EQ(Rows->Num(), 1);
	const TSharedPtr<FJsonObject> Row = (*Rows)[0]->AsObject();
	UNTEST_ASSERT_TRUE(Row.IsValid());

	int32 OriginCount = 0;
	UNTEST_EXPECT_TRUE(Row->TryGetNumberField(TEXT("origin_count"), OriginCount));
	UNTEST_EXPECT_EQ(OriginCount, 2);

	FString ClassName;
	FString UnusedBranch;
	UNTEST_EXPECT_TRUE(Row->TryGetStringField(TEXT("class"), ClassName));
	UNTEST_EXPECT_TRUE(ClassName != TEXT("all-branches"));
	UNTEST_EXPECT_FALSE(Row->TryGetStringField(TEXT("branch_node"), UnusedBranch));
	co_return;
}

// Assign the nearer qualifying branch a higher GUID to distinguish distance-first selection from GUID-first.
UNTEST_UNIT_OPTS(Claireon, LintGraphRules, ExecJoin_EvidenceIsDeterministic, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = LintGraphRule_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	// Both outer routes take two hops; both inner routes take one.
	// A direct outer route would tie the candidates and test only GUID ordering.
	UK2Node_IfThenElse* Outer = LintGraphRule_AddExecNode<UK2Node_IfThenElse>(Graph, 0, 0);
	UK2Node_IfThenElse* Inner = LintGraphRule_AddExecNode<UK2Node_IfThenElse>(Graph, 400, 0);
	UK2Node_CallFunction* Mid = LintGraphRule_AddImpureCall(Graph, 400, 400);
	UK2Node_CallFunction* Shared = LintGraphRule_AddImpureCall(Graph, 800, 0);
	UEdGraphPin* SharedIn = LintGraphRule_ExecPin(Shared, EGPD_Input);
	UNTEST_ASSERT_TRUE(SharedIn != nullptr);
	LintGraphRule_Link(LintGraphRule_ExecPin(Outer, EGPD_Output, TEXT("then")),
		LintGraphRule_ExecPin(Inner, EGPD_Input));
	LintGraphRule_Link(LintGraphRule_ExecPin(Outer, EGPD_Output, TEXT("else")),
		LintGraphRule_ExecPin(Mid, EGPD_Input));
	LintGraphRule_Link(LintGraphRule_ExecPin(Mid, EGPD_Output), SharedIn);
	LintGraphRule_Link(LintGraphRule_ExecPin(Inner, EGPD_Output, TEXT("then")), SharedIn);
	LintGraphRule_Link(LintGraphRule_ExecPin(Inner, EGPD_Output, TEXT("else")), SharedIn);

	// Assign GUIDs explicitly so the distance-versus-identity test is deterministic.
	Outer->NodeGuid = FGuid(0x0A0A0A0A, 0x0A0A0A0A, 0x0A0A0A0A, 0x0A0A0A0A);
	Inner->NodeGuid = FGuid(0xF0F0F0F0, 0xF0F0F0F0, 0xF0F0F0F0, 0xF0F0F0F0);
	const FString OuterGuid = Outer->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
	const FString InnerGuid = Inner->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
	UNTEST_ASSERT_TRUE(InnerGuid > OuterGuid);

	UK2Node_CallFunction* TailOriginA = LintGraphRule_AddImpureCall(Graph, 0, 800);
	UK2Node_CallFunction* TailOriginB = LintGraphRule_AddImpureCall(Graph, 0, 1200);
	UK2Node_CallFunction* Tail = LintGraphRule_AddImpureCall(Graph, 400, 1000);
	UEdGraphPin* TailIn = LintGraphRule_ExecPin(Tail, EGPD_Input);
	LintGraphRule_Link(LintGraphRule_ExecPin(TailOriginA, EGPD_Output), TailIn);
	LintGraphRule_Link(LintGraphRule_ExecPin(TailOriginB, EGPD_Output), TailIn);

	UK2Node_CallFunction* ExitA = LintGraphRule_AddImpureCall(Graph, 0, 1600);
	UK2Node_CallFunction* ExitB = LintGraphRule_AddImpureCall(Graph, 0, 2000);
	UK2Node_FunctionResult* Result = LintGraphRule_AddExecNode<UK2Node_FunctionResult>(Graph, 400, 1800);
	UEdGraphPin* ResultIn = LintGraphRule_ExecPin(Result, EGPD_Input);
	LintGraphRule_Link(LintGraphRule_ExecPin(ExitA, EGPD_Output), ResultIn);
	LintGraphRule_Link(LintGraphRule_ExecPin(ExitB, EGPD_Output), ResultIn);

	const TArray<FClaireonLintFinding> FirstRun = LintGraphRule_Run(Graph);
	const TArray<FClaireonLintFinding> SecondRun = LintGraphRule_Run(Graph);
	const FClaireonLintFinding* First = LintGraphRule_FindRule(FirstRun, TEXT("exec-join"));
	const FClaireonLintFinding* Second = LintGraphRule_FindRule(SecondRun, TEXT("exec-join"));
	UNTEST_ASSERT_TRUE(First != nullptr && Second != nullptr);

	const TArray<TSharedPtr<FJsonValue>>* FirstRows = LintGraphRule_JoinRows(*First);
	const TArray<TSharedPtr<FJsonValue>>* SecondRows = LintGraphRule_JoinRows(*Second);
	UNTEST_ASSERT_TRUE(FirstRows != nullptr && SecondRows != nullptr);

	UNTEST_ASSERT_EQ(FirstRows->Num(), 3);
	UNTEST_ASSERT_EQ(SecondRows->Num(), FirstRows->Num());
	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*First, TEXT("all-branches")), 1);
	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*First, TEXT("terminal-single-call-tail")), 1);
	UNTEST_EXPECT_EQ(LintGraphRule_ByClass(*First, TEXT("function-result")), 1);

	int32 AllBranchesRowsSeen = 0;
	for (const TSharedPtr<FJsonValue>& RowValue : *FirstRows)
	{
		const TSharedPtr<FJsonObject> Row = RowValue.IsValid() ? RowValue->AsObject() : nullptr;
		if (!Row.IsValid())
		{
			continue;
		}
		FString ClassName;
		if (!Row->TryGetStringField(TEXT("class"), ClassName) || ClassName != TEXT("all-branches"))
		{
			continue;
		}
		++AllBranchesRowsSeen;

		int32 CandidateCount = 0;
		UNTEST_EXPECT_TRUE(Row->TryGetNumberField(TEXT("branch_candidate_count"), CandidateCount));
		UNTEST_EXPECT_EQ(CandidateCount, 2);

		FString BranchNode;
		UNTEST_EXPECT_TRUE(Row->TryGetStringField(TEXT("branch_node"), BranchNode));
		UNTEST_EXPECT_TRUE(BranchNode == InnerGuid);
		UNTEST_EXPECT_FALSE(BranchNode == OuterGuid);

		int32 BranchDistance = 0;
		UNTEST_EXPECT_TRUE(Row->TryGetNumberField(TEXT("branch_exec_distance"), BranchDistance));
		UNTEST_EXPECT_EQ(BranchDistance, 1);
	}
	// Require a matched row so a class rename cannot bypass assertions.
	UNTEST_EXPECT_EQ(AllBranchesRowsSeen, 1);

	for (int32 Index = 0; Index < FirstRows->Num(); ++Index)
	{
		const FString FirstJson = LintGraphRule_RowToJson((*FirstRows)[Index]);
		const FString SecondJson = LintGraphRule_RowToJson((*SecondRows)[Index]);
		UNTEST_EXPECT_FALSE(FirstJson.IsEmpty());
		UNTEST_EXPECT_TRUE(FirstJson == SecondJson);
	}

	UNTEST_EXPECT_TRUE(First->Target == Second->Target);
	co_return;
}

#endif // WITH_UNTESTED
