// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test duplicate-chain signatures and threshold boundaries.
// Matching uses class, member reference, and pin order; literal values and display titles are excluded.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonLintTypes.h"

#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Knot.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace ClaireonLintDupTestsInternal
{
	// Prefix helpers to avoid unity-build collisions.

	struct FDupFixture
	{
		UBlueprint* Blueprint = nullptr;
		UEdGraph* Graph = nullptr;
	};

	static FDupFixture Dup_MakeFixture()
	{
		FDupFixture Fixture;
		Fixture.Blueprint = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), GetTransientPackage(), NAME_None, BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass());
		if (IsValid(Fixture.Blueprint) && Fixture.Blueprint->UbergraphPages.Num() > 0)
		{
			Fixture.Graph = Fixture.Blueprint->UbergraphPages[0];
			// Remove template events so they cannot join fixture chains.
			Fixture.Graph->Nodes.Empty();
		}
		return Fixture;
	}

	/** Use stock PrintString calls with real exec and data pins. */
	static UK2Node_CallFunction* Dup_AddCall(UEdGraph* Graph, const TCHAR* FunctionName)
	{
		UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph, NAME_None, RF_Transient);
		Node->FunctionReference.SetExternalMember(
			FName(FunctionName), UKismetSystemLibrary::StaticClass());
		Graph->Nodes.Add(Node);
		Node->CreateNewGuid();
		Node->AllocateDefaultPins();
		return Node;
	}

	static UEdGraphPin* Dup_ExecOut(UEdGraphNode* Node)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Output
				&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static UEdGraphPin* Dup_ExecIn(UEdGraphNode* Node)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == EGPD_Input
				&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static void Dup_Link(UEdGraphNode* From, UEdGraphNode* To)
	{
		UEdGraphPin* Out = Dup_ExecOut(From);
		UEdGraphPin* In = Dup_ExecIn(To);
		if (Out && In)
		{
			Out->MakeLinkTo(In);
		}
	}

	/** A linear chain of Length CallFunction nodes, all bound to FunctionName. */
	static TArray<UK2Node_CallFunction*> Dup_AddChain(UEdGraph* Graph, const TCHAR* FunctionName,
	                                                  int32 Length)
	{
		TArray<UK2Node_CallFunction*> Nodes;
		for (int32 i = 0; i < Length; ++i)
		{
			UK2Node_CallFunction* Node = Dup_AddCall(Graph, FunctionName);
			if (i > 0)
			{
				Dup_Link(Nodes[i - 1], Node);
			}
			Nodes.Add(Node);
		}
		return Nodes;
	}

	static int32 Dup_CountRule(const TArray<FClaireonLintFinding>& Findings)
	{
		int32 Count = 0;
		for (const FClaireonLintFinding& Finding : Findings)
		{
			if (Finding.Rule == TEXT("duplicate-subgraph"))
			{
				++Count;
			}
		}
		return Count;
	}

	static TArray<FClaireonLintFinding> Dup_Run(const FDupFixture& Fixture)
	{
		TArray<FClaireonLintFinding> Findings;
		FClaireonLintContext Context;
		Context.Graph = Fixture.Graph;
		Context.Blueprint = Fixture.Blueprint;
		ClaireonLint::RunGraphRules(Context, Findings);
		return Findings;
	}
}

// Report one finding per duplicate group.
UNTEST_UNIT_OPTS(Claireon, LintDuplicateSubgraph, DuplicateSubgraph_TwoIdenticalChainsReportOnce,
	UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLintDupTestsInternal;
	FDupFixture Fixture = Dup_MakeFixture();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Blueprint));
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	const int32 Min = FClaireonLintThresholds().DuplicateChainMin;
	Dup_AddChain(Fixture.Graph, TEXT("PrintString"), Min);
	Dup_AddChain(Fixture.Graph, TEXT("PrintString"), Min);

	TArray<FClaireonLintFinding> Findings = Dup_Run(Fixture);
	UNTEST_EXPECT_EQ(Dup_CountRule(Findings), 1);

	for (const FClaireonLintFinding& Finding : Findings)
	{
		if (Finding.Rule != TEXT("duplicate-subgraph"))
		{
			continue;
		}
		UNTEST_EXPECT_TRUE(Finding.Severity == EClaireonLintSeverity::Info);
		UNTEST_EXPECT_TRUE(Finding.Confidence == EClaireonLintConfidence::Medium);
		// Extraction requires naming and boundary decisions, so no mechanical fix is offered.
		UNTEST_EXPECT_FALSE(Finding.SuggestedFix.IsValid());
		UNTEST_ASSERT_TRUE(Finding.Evidence.IsValid());

		double Occurrences = 0.0;
		UNTEST_EXPECT_TRUE(Finding.Evidence->TryGetNumberField(TEXT("occurrences"), Occurrences));
		UNTEST_EXPECT_EQ(static_cast<int32>(Occurrences), 2);
		double Length = 0.0;
		UNTEST_EXPECT_TRUE(Finding.Evidence->TryGetNumberField(TEXT("chain_length"), Length));
		UNTEST_EXPECT_EQ(static_cast<int32>(Length), Min);
	}

	co_return;
}

// Equal-length chains with different member references must not match.
UNTEST_UNIT_OPTS(Claireon, LintDuplicateSubgraph, DuplicateSubgraph_DifferentFunctionsDoNotMatch,
	UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLintDupTestsInternal;
	FDupFixture Fixture = Dup_MakeFixture();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	const int32 Min = FClaireonLintThresholds().DuplicateChainMin;
	Dup_AddChain(Fixture.Graph, TEXT("PrintString"), Min);
	Dup_AddChain(Fixture.Graph, TEXT("PrintText"), Min);

	UNTEST_EXPECT_EQ(Dup_CountRule(Dup_Run(Fixture)), 0);
	co_return;
}

// Exercise threshold minus one, threshold, and threshold plus one.
UNTEST_UNIT_OPTS(Claireon, LintDuplicateSubgraph, DuplicateSubgraph_ThresholdBoundary,
	UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLintDupTestsInternal;
	const int32 Min = FClaireonLintThresholds().DuplicateChainMin;

	// threshold - 1: below the bar, silent.
	{
		FDupFixture Fixture = Dup_MakeFixture();
		UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));
		Dup_AddChain(Fixture.Graph, TEXT("PrintString"), Min - 1);
		Dup_AddChain(Fixture.Graph, TEXT("PrintString"), Min - 1);
		UNTEST_EXPECT_EQ(Dup_CountRule(Dup_Run(Fixture)), 0);
	}

	{
		FDupFixture Fixture = Dup_MakeFixture();
		UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));
		Dup_AddChain(Fixture.Graph, TEXT("PrintString"), Min);
		Dup_AddChain(Fixture.Graph, TEXT("PrintString"), Min);
		UNTEST_EXPECT_EQ(Dup_CountRule(Dup_Run(Fixture)), 1);
	}

	{
		FDupFixture Fixture = Dup_MakeFixture();
		UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));
		Dup_AddChain(Fixture.Graph, TEXT("PrintString"), Min + 1);
		Dup_AddChain(Fixture.Graph, TEXT("PrintString"), Min + 1);
		TArray<FClaireonLintFinding> Findings = Dup_Run(Fixture);
		UNTEST_ASSERT_EQ(Dup_CountRule(Findings), 1);
		for (const FClaireonLintFinding& Finding : Findings)
		{
			if (Finding.Rule == TEXT("duplicate-subgraph") && Finding.Evidence.IsValid())
			{
				double Length = 0.0;
				UNTEST_EXPECT_TRUE(Finding.Evidence->TryGetNumberField(TEXT("chain_length"), Length));
				UNTEST_EXPECT_EQ(static_cast<int32>(Length), Min + 1);
			}
		}
	}

	co_return;
}

// A single chain must not form a duplicate group.
UNTEST_UNIT_OPTS(Claireon, LintDuplicateSubgraph, DuplicateSubgraph_LoneChainIsSilent,
	UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLintDupTestsInternal;
	FDupFixture Fixture = Dup_MakeFixture();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	Dup_AddChain(Fixture.Graph, TEXT("PrintString"), FClaireonLintThresholds().DuplicateChainMin + 3);
	UNTEST_EXPECT_EQ(Dup_CountRule(Dup_Run(Fixture)), 0);
	co_return;
}

// A reroute must not hide duplicate structure.
UNTEST_UNIT_OPTS(Claireon, LintDuplicateSubgraph, DuplicateSubgraph_ARerouteDoesNotHideIt,
	UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLintDupTestsInternal;
	FDupFixture Fixture = Dup_MakeFixture();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	const int32 Min = FClaireonLintThresholds().DuplicateChainMin;
	Dup_AddChain(Fixture.Graph, TEXT("PrintString"), Min);

	TArray<UK2Node_CallFunction*> Second;
	for (int32 i = 0; i < Min; ++i)
	{
		Second.Add(Dup_AddCall(Fixture.Graph, TEXT("PrintString")));
	}
	UK2Node_Knot* Knot = NewObject<UK2Node_Knot>(Fixture.Graph, NAME_None, RF_Transient);
	Fixture.Graph->Nodes.Add(Knot);
	Knot->CreateNewGuid();
	Knot->AllocateDefaultPins();

	if (UEdGraphPin* Out = Dup_ExecOut(Second[0]); Out && Knot->GetInputPin())
	{
		Out->MakeLinkTo(Knot->GetInputPin());
	}
	if (Knot->GetOutputPin())
	{
		if (UEdGraphPin* In = Dup_ExecIn(Second[1]))
		{
			Knot->GetOutputPin()->MakeLinkTo(In);
		}
	}
	for (int32 i = 2; i < Min; ++i)
	{
		Dup_Link(Second[i - 1], Second[i]);
	}

	UNTEST_EXPECT_EQ(Dup_CountRule(Dup_Run(Fixture)), 1);
	co_return;
}

// Require stable finding order across runs.
UNTEST_UNIT_OPTS(Claireon, LintDuplicateSubgraph, DuplicateSubgraph_OrderIsDeterministic,
	UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLintDupTestsInternal;
	FDupFixture Fixture = Dup_MakeFixture();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	const int32 Min = FClaireonLintThresholds().DuplicateChainMin;
	// Use multiple groups so ordering is observable.
	Dup_AddChain(Fixture.Graph, TEXT("PrintString"), Min);
	Dup_AddChain(Fixture.Graph, TEXT("PrintString"), Min);
	Dup_AddChain(Fixture.Graph, TEXT("PrintText"), Min);
	Dup_AddChain(Fixture.Graph, TEXT("PrintText"), Min);
	Dup_AddChain(Fixture.Graph, TEXT("PrintWarning"), Min);
	Dup_AddChain(Fixture.Graph, TEXT("PrintWarning"), Min);

	TArray<FClaireonLintFinding> First = Dup_Run(Fixture);
	TArray<FClaireonLintFinding> Second = Dup_Run(Fixture);
	UNTEST_ASSERT_EQ(First.Num(), Second.Num());
	UNTEST_EXPECT_GE(Dup_CountRule(First), 3);

	for (int32 i = 0; i < First.Num(); ++i)
	{
		UNTEST_EXPECT_STREQ(*First[i].Rule, *Second[i].Rule);
		UNTEST_EXPECT_STREQ(*First[i].Target, *Second[i].Target);
	}
	co_return;
}

#endif // WITH_UNTESTED
