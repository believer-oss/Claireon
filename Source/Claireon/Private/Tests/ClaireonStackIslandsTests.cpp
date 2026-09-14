// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Headless island-stacking tests inspect positions, connectivity, and result fields on Blueprint-owned fixtures.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonBlueprintGraphTool_StackIslands.h"
#include "ClaireonGraphIslands.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_Event.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace ClaireonStackIslandsTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	using ClaireonGraphIslands::FIsland;
	using namespace ClaireonStackIslands;

	/** Use a dedicated page to exclude template events from island counts and rail position. */
	UEdGraph* StackIslands_MakeGraph()
	{
		static int32 Counter = 0;
		const FString AssetName = FString::Printf(TEXT("BP_StackIslands_%d"), Counter++);
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
			BP, FName(TEXT("StackFixtureGraph")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (IsValid(Graph))
		{
			FBlueprintEditorUtils::AddUbergraphPage(BP, Graph);
		}
		return Graph;
	}

	/** The engine's own graph -> Blueprint accessor, rather than an assumption about the outer. */
	UBlueprint* StackIslands_BlueprintOf(UEdGraph* Graph)
	{
		return IsValid(Graph) ? FBlueprintEditorUtils::FindBlueprintForGraph(Graph) : nullptr;
	}

	template <typename TNode>
	TNode* StackIslands_AddNode(UEdGraph* Graph, int32 X, int32 Y)
	{
		TNode* Node = NewObject<TNode>(Graph, NAME_None, RF_Transient);
		Graph->Nodes.Add(Node);
		Node->CreateNewGuid();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		return Node;
	}

	/** Create the synthetic event's exec pin directly; no real function signature is needed. */
	UK2Node_Event* StackIslands_AddEvent(UEdGraph* Graph, const TCHAR* EventName, int32 X, int32 Y)
	{
		UK2Node_Event* Event = StackIslands_AddNode<UK2Node_Event>(Graph, X, Y);
		Event->CustomFunctionName = FName(EventName);
		Event->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Then);
		return Event;
	}

	/** An ordinary island member: impure call, exec in and out. */
	UK2Node_CallFunction* StackIslands_AddCall(UEdGraph* Graph, int32 X, int32 Y)
	{
		UK2Node_CallFunction* Call = StackIslands_AddNode<UK2Node_CallFunction>(Graph, X, Y);
		Call->FunctionReference.SetExternalMember(
			FName(TEXT("PrintString")), UKismetSystemLibrary::StaticClass());
		Call->PostPlacedNewNode();
		Call->AllocateDefaultPins();
		return Call;
	}

	/** A data feeder left of an entry distinguishes entry alignment from box-minimum alignment. */
	UK2Node_VariableGet* StackIslands_AddFeeder(UEdGraph* Graph, const TCHAR* VarName, int32 X, int32 Y)
	{
		UK2Node_VariableGet* Get = StackIslands_AddNode<UK2Node_VariableGet>(Graph, X, Y);
		Get->VariableReference.SetSelfMember(FName(VarName));
		Get->PostPlacedNewNode();
		Get->AllocateDefaultPins();
		// Use a synthetic output because the fixture variable is undeclared.
		if (Get->Pins.Num() == 0)
		{
			Get->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Int, FName(VarName));
		}
		return Get;
	}

	UEdGraphNode_Comment* StackIslands_AddComment(UEdGraph* Graph, int32 X, int32 Y, int32 Width, int32 Height)
	{
		UEdGraphNode_Comment* Comment = StackIslands_AddNode<UEdGraphNode_Comment>(Graph, X, Y);
		Comment->NodeWidth = Width;
		Comment->NodeHeight = Height;
		return Comment;
	}

	UEdGraphPin* StackIslands_FirstPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
	{
		if (!IsValid(Node))
		{
			return nullptr;
		}
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Direction)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	/** Joins two nodes into one island. Category is irrelevant: the island builder walks links of any kind. */
	void StackIslands_Connect(UEdGraphNode* From, UEdGraphNode* To)
	{
		UEdGraphPin* Out = StackIslands_FirstPin(From, EGPD_Output);
		UEdGraphPin* In = StackIslands_FirstPin(To, EGPD_Input);
		if (Out && In)
		{
			Out->LinkedTo.AddUnique(In);
			In->LinkedTo.AddUnique(Out);
		}
	}

	TMap<FGuid, FIntPoint> StackIslands_Snapshot(UEdGraph* Graph)
	{
		TMap<FGuid, FIntPoint> Positions;
		for (const UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsValid(Node))
			{
				Positions.Add(Node->NodeGuid, FIntPoint(Node->NodePosX, Node->NodePosY));
			}
		}
		return Positions;
	}

	TArray<FIsland> StackIslands_Islands(UEdGraph* Graph)
	{
		TArray<FIsland> Islands;
		ClaireonGraphIslands::Build(Graph, Islands);
		return Islands;
	}

	const FIsland* StackIslands_IslandHolding(const TArray<FIsland>& Islands, const UEdGraphNode* Node)
	{
		for (const FIsland& Island : Islands)
		{
			if (Island.Contains(Node))
			{
				return &Island;
			}
		}
		return nullptr;
	}

	const FIsland* StackIslands_IslandByRep(const TArray<FIsland>& Islands, const FString& Representative)
	{
		for (const FIsland& Island : Islands)
		{
			if (Island.Representative == Representative)
			{
				return &Island;
			}
		}
		return nullptr;
	}

	const FPlacedIsland* StackIslands_PlacedByRep(const FStackReport& Report, const FString& Representative)
	{
		for (const FPlacedIsland& Row : Report.IslandsPlaced)
		{
			if (Row.Representative == Representative)
			{
				return &Row;
			}
		}
		return nullptr;
	}

	const FSkippedIsland* StackIslands_SkippedByRep(const FStackReport& Report, const FString& Representative)
	{
		for (const FSkippedIsland& Row : Report.IslandsSkipped)
		{
			if (Row.Representative == Representative)
			{
				return &Row;
			}
		}
		return nullptr;
	}

	/** Require exact pairwise integer offsets after rigid translation. */
	bool StackIslands_RigidityHolds(
		const TArray<FIsland>& IslandsBefore,
		const TMap<FGuid, FIntPoint>& Before,
		const TMap<FGuid, FIntPoint>& After,
		FString& OutWhy)
	{
		for (const FIsland& Island : IslandsBefore)
		{
			for (int32 I = 0; I < Island.Nodes.Num(); ++I)
			{
				for (int32 J = I + 1; J < Island.Nodes.Num(); ++J)
				{
					const FGuid GuidA = Island.Nodes[I]->NodeGuid;
					const FGuid GuidB = Island.Nodes[J]->NodeGuid;
					const FIntPoint* BeforeA = Before.Find(GuidA);
					const FIntPoint* BeforeB = Before.Find(GuidB);
					const FIntPoint* AfterA = After.Find(GuidA);
					const FIntPoint* AfterB = After.Find(GuidB);
					if (!BeforeA || !BeforeB || !AfterA || !AfterB)
					{
						OutWhy = FString::Printf(TEXT("island %s lost a member between snapshots"),
							*Island.Representative);
						return false;
					}

					const FIntPoint OffsetBefore = *BeforeA - *BeforeB;
					const FIntPoint OffsetAfter = *AfterA - *AfterB;
					if (OffsetBefore != OffsetAfter)
					{
						OutWhy = FString::Printf(
							TEXT("island %s: offset between %s and %s was (%d,%d) and is now (%d,%d)"),
							*Island.Representative,
							*GuidA.ToString(EGuidFormats::DigitsWithHyphens),
							*GuidB.ToString(EGuidFormats::DigitsWithHyphens),
							OffsetBefore.X, OffsetBefore.Y, OffsetAfter.X, OffsetAfter.Y);
						return false;
					}
				}
			}
		}
		return true;
	}

	/** Two islands overlap in both axes; one needs horizontal movement and the other vertical movement. */
	struct FTwoIslandFixture
	{
		UEdGraph* Graph = nullptr;
		UK2Node_Event* EntryA = nullptr;
		UK2Node_CallFunction* CallA = nullptr;
		UK2Node_Event* EntryB = nullptr;
		UK2Node_CallFunction* CallB = nullptr;
	};

	FTwoIslandFixture StackIslands_MakeTwoIslands()
	{
		FTwoIslandFixture Fixture;
		Fixture.Graph = StackIslands_MakeGraph();
		if (!IsValid(Fixture.Graph))
		{
			return Fixture;
		}

		Fixture.EntryA = StackIslands_AddEvent(Fixture.Graph, TEXT("StackEventA"), 100, 0);
		Fixture.CallA = StackIslands_AddCall(Fixture.Graph, 400, 400);
		StackIslands_Connect(Fixture.EntryA, Fixture.CallA);

		Fixture.EntryB = StackIslands_AddEvent(Fixture.Graph, TEXT("StackEventB"), 0, 200);
		Fixture.CallB = StackIslands_AddCall(Fixture.Graph, 300, 600);
		StackIslands_Connect(Fixture.EntryB, Fixture.CallB);

		return Fixture;
	}
}

using namespace ClaireonStackIslandsTestsNS;
using namespace ClaireonStackIslands;

// Stack overlapping islands into a column.
UNTEST_UNIT_OPTS(Claireon, StackIslands, OverlappingIslandsBecomeDisjointColumn, UNTEST_TIMEOUTMS(30000))
{
	FTwoIslandFixture Fixture = StackIslands_MakeTwoIslands();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	const TArray<FIsland> Before = StackIslands_Islands(Fixture.Graph);
	UNTEST_ASSERT_EQ(Before.Num(), 2);
	UNTEST_ASSERT_EQ(Before[0].Nodes.Num(), 2);
	UNTEST_ASSERT_EQ(Before[1].Nodes.Num(), 2);

	const FIsland* IslandA = StackIslands_IslandHolding(Before, Fixture.EntryA);
	const FIsland* IslandB = StackIslands_IslandHolding(Before, Fixture.EntryB);
	UNTEST_ASSERT_TRUE(IslandA != nullptr && IslandB != nullptr);
	UNTEST_ASSERT_TRUE(IslandA->Box.Intersects(IslandB->Box));

	const FString RepA = IslandA->Representative;
	const FString RepB = IslandB->Representative;

	FStackRequest Request;
	FStackReport Report;
	UNTEST_ASSERT_TRUE(Apply(StackIslands_BlueprintOf(Fixture.Graph), Fixture.Graph, Request, Report));
	UNTEST_ASSERT_TRUE(Report.Error.IsEmpty());

	UNTEST_EXPECT_NEAR(Report.RailX, 0.0, 0.001);
	UNTEST_EXPECT_NEAR(Report.Gutter, ClaireonStackIslands::DefaultGutter, 0.001);
	UNTEST_ASSERT_EQ(Report.IslandsPlaced.Num(), 2);
	UNTEST_EXPECT_EQ(Report.IslandsSkipped.Num(), 0);
	UNTEST_EXPECT_EQ(Report.NodesMoved, 4);

	const TArray<FIsland> After = StackIslands_Islands(Fixture.Graph);
	const FIsland* AfterA = StackIslands_IslandByRep(After, RepA);
	const FIsland* AfterB = StackIslands_IslandByRep(After, RepB);
	UNTEST_ASSERT_TRUE(AfterA != nullptr && AfterB != nullptr);

	// Align primary entries, allowing feeders to extend left of the rail.
	UNTEST_EXPECT_NEAR(ClaireonGraphIslands::ResolveIslandEntryX(*AfterA), Report.RailX, 0.001);
	UNTEST_EXPECT_NEAR(ClaireonGraphIslands::ResolveIslandEntryX(*AfterB), Report.RailX, 0.001);

	UNTEST_EXPECT_TRUE(AfterA->Box.MaxY < AfterB->Box.MinY);
	UNTEST_EXPECT_FALSE(AfterA->Box.Intersects(AfterB->Box));
	UNTEST_EXPECT_NEAR(AfterB->Box.MinY - AfterA->Box.MaxY, ClaireonStackIslands::DefaultGutter, 0.001);
	co_return;
}

// Round one shared delta, not each node position, to preserve pairwise offsets at fractional rails.
UNTEST_UNIT_OPTS(Claireon, StackIslands, RigidTranslationPreservesEveryPairwiseOffset, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = StackIslands_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_Event* EntryA = StackIslands_AddEvent(Graph, TEXT("RigidEventA"), 100, 0);
	UK2Node_CallFunction* CallA = StackIslands_AddCall(Graph, 400, 40);
	UK2Node_VariableGet* FeederA = StackIslands_AddFeeder(Graph, TEXT("RigidFeedA"), 75, 120);
	StackIslands_Connect(EntryA, CallA);
	StackIslands_Connect(FeederA, CallA);

	UK2Node_Event* EntryB = StackIslands_AddEvent(Graph, TEXT("RigidEventB"), 0, 300);
	UK2Node_CallFunction* CallB = StackIslands_AddCall(Graph, 300, 340);
	StackIslands_Connect(EntryB, CallB);

	const TArray<FIsland> Before = StackIslands_Islands(Graph);
	UNTEST_ASSERT_EQ(Before.Num(), 2);
	const FIsland* IslandA = StackIslands_IslandHolding(Before, EntryA);
	UNTEST_ASSERT_TRUE(IslandA != nullptr);
	UNTEST_ASSERT_EQ(IslandA->Nodes.Num(), 3);

	const TMap<FGuid, FIntPoint> BeforePositions = StackIslands_Snapshot(Graph);

	FStackRequest Request;
	Request.RailX = 12.5;
	Request.bRailXExplicit = true;

	FStackReport Report;
	UNTEST_ASSERT_TRUE(Apply(StackIslands_BlueprintOf(Graph), Graph, Request, Report));
	UNTEST_ASSERT_EQ(Report.IslandsPlaced.Num(), 2);
	UNTEST_ASSERT_EQ(Report.NodesMoved, 5);

	const TMap<FGuid, FIntPoint> AfterPositions = StackIslands_Snapshot(Graph);

	FString Why;
	const bool bRigid = StackIslands_RigidityHolds(Before, BeforePositions, AfterPositions, Why);
	if (!bRigid)
	{
		UE_LOG(LogTemp, Error, TEXT("[StackIslands] rigidity violated: %s"), *Why);
	}
	UNTEST_ASSERT_TRUE(bRigid);

	// The shared delta -87.5 rounds to -87.
	const FPlacedIsland* PlacedA = StackIslands_PlacedByRep(Report, IslandA->Representative);
	UNTEST_ASSERT_TRUE(PlacedA != nullptr);
	UNTEST_EXPECT_EQ(PlacedA->DeltaX, -87);
	UNTEST_EXPECT_EQ(EntryA->NodePosX, 13);
	UNTEST_EXPECT_EQ(FeederA->NodePosX, -12);
	UNTEST_EXPECT_EQ(CallA->NodePosX, 313);
	co_return;
}

// Carry single-island comments with their island.
UNTEST_UNIT_OPTS(Claireon, StackIslands, CommentOverOneIslandIsCarriedWithIt, UNTEST_TIMEOUTMS(30000))
{
	FTwoIslandFixture Fixture = StackIslands_MakeTwoIslands();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	// X[50,550] Y[-50,500]: holds A's two anchors (100,0) and (400,400), and neither of B's
	// -- (0,200) is left of the box and (300,600) is below it.
	UEdGraphNode_Comment* Comment = StackIslands_AddComment(Fixture.Graph, 50, -50, 500, 550);

	const TArray<FIsland> Before = StackIslands_Islands(Fixture.Graph);
	UNTEST_ASSERT_EQ(Before.Num(), 2);
	const FIsland* IslandA = StackIslands_IslandHolding(Before, Fixture.EntryA);
	UNTEST_ASSERT_TRUE(IslandA != nullptr);
	const FString RepA = IslandA->Representative;

	FStackRequest Request;
	FStackReport Report;
	UNTEST_ASSERT_TRUE(Apply(StackIslands_BlueprintOf(Fixture.Graph), Fixture.Graph, Request, Report));

	UNTEST_ASSERT_EQ(Report.CommentsCarried.Num(), 1);
	UNTEST_EXPECT_EQ(Report.CommentsSkipped.Num(), 0);
	UNTEST_EXPECT_STREQ(Report.CommentsCarried[0].CommentGuid,
		Comment->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	UNTEST_EXPECT_STREQ(Report.CommentsCarried[0].Island, RepA);

	const FPlacedIsland* PlacedA = StackIslands_PlacedByRep(Report, RepA);
	UNTEST_ASSERT_TRUE(PlacedA != nullptr);
	UNTEST_EXPECT_EQ(PlacedA->DeltaX, -100);
	UNTEST_EXPECT_EQ(PlacedA->DeltaY, 0);
	UNTEST_EXPECT_EQ(Comment->NodePosX, 50 + PlacedA->DeltaX);
	UNTEST_EXPECT_EQ(Comment->NodePosY, -50 + PlacedA->DeltaY);
	co_return;
}

// Leave multi-island comments in place while stacking their islands.
UNTEST_UNIT_OPTS(Claireon, StackIslands, CommentSpanningTwoIslandsIsRefusedAndNamed, UNTEST_TIMEOUTMS(30000))
{
	FTwoIslandFixture Fixture = StackIslands_MakeTwoIslands();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	// X[-50,550] Y[-50,750]: holds every anchor in both islands.
	UEdGraphNode_Comment* Comment = StackIslands_AddComment(Fixture.Graph, -50, -50, 600, 800);

	const TArray<FIsland> Before = StackIslands_Islands(Fixture.Graph);
	UNTEST_ASSERT_EQ(Before.Num(), 2);
	const FIsland* IslandA = StackIslands_IslandHolding(Before, Fixture.EntryA);
	const FIsland* IslandB = StackIslands_IslandHolding(Before, Fixture.EntryB);
	UNTEST_ASSERT_TRUE(IslandA != nullptr && IslandB != nullptr);

	FStackRequest Request;
	FStackReport Report;
	UNTEST_ASSERT_TRUE(Apply(StackIslands_BlueprintOf(Fixture.Graph), Fixture.Graph, Request, Report));

	UNTEST_EXPECT_EQ(Report.CommentsCarried.Num(), 0);
	UNTEST_ASSERT_EQ(Report.CommentsSkipped.Num(), 1);
	UNTEST_EXPECT_STREQ(Report.CommentsSkipped[0].CommentGuid,
		Comment->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	UNTEST_EXPECT_STREQ(Report.CommentsSkipped[0].PolicyReason, ClaireonStackIslands::PolicySpanningComment());
	UNTEST_ASSERT_EQ(Report.CommentsSkipped[0].SpansIslands.Num(), 2);
	UNTEST_EXPECT_TRUE(Report.CommentsSkipped[0].SpansIslands.Contains(IslandA->Representative));
	UNTEST_EXPECT_TRUE(Report.CommentsSkipped[0].SpansIslands.Contains(IslandB->Representative));

	UNTEST_EXPECT_EQ(Comment->NodePosX, -50);
	UNTEST_EXPECT_EQ(Comment->NodePosY, -50);

	UNTEST_EXPECT_EQ(Report.IslandsPlaced.Num(), 2);
	UNTEST_EXPECT_EQ(Report.NodesMoved, 4);
	co_return;
}

// Skipped singletons consume no slot and do not shift the column start.
UNTEST_UNIT_OPTS(Claireon, StackIslands, SingletonIsSkippedByDefaultAndReported, UNTEST_TIMEOUTMS(30000))
{
	FTwoIslandFixture Fixture = StackIslands_MakeTwoIslands();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	// Use a non-entry singleton so it cannot change the rail.
	UK2Node_VariableGet* Parked = StackIslands_AddFeeder(Fixture.Graph, TEXT("ParkedName"), -2000, -2000);

	const TArray<FIsland> Before = StackIslands_Islands(Fixture.Graph);
	UNTEST_ASSERT_EQ(Before.Num(), 3);
	const FIsland* ParkedIsland = StackIslands_IslandHolding(Before, Parked);
	UNTEST_ASSERT_TRUE(ParkedIsland != nullptr);
	UNTEST_ASSERT_EQ(ParkedIsland->Nodes.Num(), 1);
	const FString RepParked = ParkedIsland->Representative;

	FStackRequest Request;
	FStackReport Report;
	UNTEST_ASSERT_TRUE(Apply(StackIslands_BlueprintOf(Fixture.Graph), Fixture.Graph, Request, Report));

	UNTEST_EXPECT_EQ(Report.IslandsTotal, 3);
	UNTEST_EXPECT_EQ(Report.IslandsPlaced.Num(), 2);
	UNTEST_ASSERT_EQ(Report.IslandsSkipped.Num(), 1);

	const FSkippedIsland* SkippedRow = StackIslands_SkippedByRep(Report, RepParked);
	UNTEST_ASSERT_TRUE(SkippedRow != nullptr);
	UNTEST_EXPECT_STREQ(SkippedRow->Reason, ClaireonStackIslands::PolicySingleton());
	UNTEST_EXPECT_EQ(SkippedRow->NodeCount, 1);

	UNTEST_EXPECT_EQ(Parked->NodePosX, -2000);
	UNTEST_EXPECT_EQ(Parked->NodePosY, -2000);

	const TArray<FIsland> After = StackIslands_Islands(Fixture.Graph);
	const FIsland* AfterA = StackIslands_IslandHolding(After, Fixture.EntryA);
	UNTEST_ASSERT_TRUE(AfterA != nullptr);
	UNTEST_EXPECT_NEAR(AfterA->Box.MinY, 0.0, 0.001);
	co_return;
}

// Excluded islands remain untouched.
UNTEST_UNIT_OPTS(Claireon, StackIslands, ExcludedIslandIsNotMoved, UNTEST_TIMEOUTMS(30000))
{
	FTwoIslandFixture Fixture = StackIslands_MakeTwoIslands();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	const TArray<FIsland> Before = StackIslands_Islands(Fixture.Graph);
	UNTEST_ASSERT_EQ(Before.Num(), 2);
	const FIsland* IslandB = StackIslands_IslandHolding(Before, Fixture.EntryB);
	UNTEST_ASSERT_TRUE(IslandB != nullptr);
	const FString RepB = IslandB->Representative;

	FStackRequest Request;
	Request.ExcludeIslands.Add(RepB);

	FStackReport Report;
	UNTEST_ASSERT_TRUE(Apply(StackIslands_BlueprintOf(Fixture.Graph), Fixture.Graph, Request, Report));

	UNTEST_EXPECT_EQ(Report.IslandsPlaced.Num(), 1);
	UNTEST_ASSERT_EQ(Report.IslandsSkipped.Num(), 1);
	const FSkippedIsland* SkippedRow = StackIslands_SkippedByRep(Report, RepB);
	UNTEST_ASSERT_TRUE(SkippedRow != nullptr);
	UNTEST_EXPECT_STREQ(SkippedRow->Reason, ClaireonStackIslands::PolicyExcludedByRequest());

	UNTEST_EXPECT_EQ(Fixture.EntryB->NodePosX, 0);
	UNTEST_EXPECT_EQ(Fixture.EntryB->NodePosY, 200);
	UNTEST_EXPECT_EQ(Fixture.CallB->NodePosX, 300);
	UNTEST_EXPECT_EQ(Fixture.CallB->NodePosY, 600);

	UNTEST_EXPECT_EQ(Report.NodesMoved, 2);
	UNTEST_EXPECT_EQ(Fixture.EntryA->NodePosX, 0);
	co_return;
}

// Reject incomplete explicit orders before moving any node.
UNTEST_UNIT_OPTS(Claireon, StackIslands, IncompleteExplicitOrderRefusesWithoutMoving, UNTEST_TIMEOUTMS(30000))
{
	FTwoIslandFixture Fixture = StackIslands_MakeTwoIslands();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	const TArray<FIsland> Before = StackIslands_Islands(Fixture.Graph);
	UNTEST_ASSERT_EQ(Before.Num(), 2);
	const FIsland* IslandA = StackIslands_IslandHolding(Before, Fixture.EntryA);
	UNTEST_ASSERT_TRUE(IslandA != nullptr);

	const TMap<FGuid, FIntPoint> BeforePositions = StackIslands_Snapshot(Fixture.Graph);

	FStackRequest Request;
	Request.Order = EStackOrder::Explicit;
	Request.IslandOrder.Add(IslandA->Representative);

	FStackReport Report;
	UNTEST_ASSERT_FALSE(Apply(StackIslands_BlueprintOf(Fixture.Graph), Fixture.Graph, Request, Report));
	UNTEST_EXPECT_FALSE(Report.Error.IsEmpty());
	UNTEST_EXPECT_STREQ(Report.RefusalReason, TEXT("bad_argument"));
	UNTEST_EXPECT_EQ(Report.NodesMoved, 0);
	UNTEST_EXPECT_EQ(Report.IslandsPlaced.Num(), 0);

	UNTEST_EXPECT_TRUE(Report.Error.Contains(TEXT("omits")));

	const TMap<FGuid, FIntPoint> AfterPositions = StackIslands_Snapshot(Fixture.Graph);
	UNTEST_ASSERT_EQ(AfterPositions.Num(), BeforePositions.Num());
	bool bAllIdentical = true;
	for (const TPair<FGuid, FIntPoint>& Pair : BeforePositions)
	{
		const FIntPoint* Now = AfterPositions.Find(Pair.Key);
		bAllIdentical &= (Now != nullptr && *Now == Pair.Value);
	}
	UNTEST_EXPECT_TRUE(bAllIdentical);
	co_return;
}

// A valid reverse order must change the column order.
UNTEST_UNIT_OPTS(Claireon, StackIslands, CompleteExplicitOrderStacksInThatOrder, UNTEST_TIMEOUTMS(30000))
{
	FTwoIslandFixture Fixture = StackIslands_MakeTwoIslands();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));

	const TArray<FIsland> Before = StackIslands_Islands(Fixture.Graph);
	UNTEST_ASSERT_EQ(Before.Num(), 2);
	const FIsland* IslandA = StackIslands_IslandHolding(Before, Fixture.EntryA);
	const FIsland* IslandB = StackIslands_IslandHolding(Before, Fixture.EntryB);
	UNTEST_ASSERT_TRUE(IslandA != nullptr && IslandB != nullptr);
	const FString RepA = IslandA->Representative;
	const FString RepB = IslandB->Representative;

	FStackRequest Request;
	Request.Order = EStackOrder::Explicit;
	Request.IslandOrder.Add(RepB);
	Request.IslandOrder.Add(RepA);

	FStackReport Report;
	UNTEST_ASSERT_TRUE(Apply(StackIslands_BlueprintOf(Fixture.Graph), Fixture.Graph, Request, Report));
	UNTEST_ASSERT_EQ(Report.IslandsPlaced.Num(), 2);
	UNTEST_EXPECT_STREQ(Report.IslandsPlaced[0].Representative, RepB);

	const TArray<FIsland> After = StackIslands_Islands(Fixture.Graph);
	const FIsland* AfterA = StackIslands_IslandByRep(After, RepA);
	const FIsland* AfterB = StackIslands_IslandByRep(After, RepB);
	UNTEST_ASSERT_TRUE(AfterA != nullptr && AfterB != nullptr);
	UNTEST_EXPECT_TRUE(AfterB->Box.MaxY < AfterA->Box.MinY);
	co_return;
}

// A second stacking pass moves nothing.
UNTEST_UNIT_OPTS(Claireon, StackIslands, SecondCallMovesNothing, UNTEST_TIMEOUTMS(30000))
{
	FTwoIslandFixture Fixture = StackIslands_MakeTwoIslands();
	UNTEST_ASSERT_TRUE(IsValid(Fixture.Graph));
	UNTEST_ASSERT_EQ(StackIslands_Islands(Fixture.Graph).Num(), 2);

	UBlueprint* Blueprint = StackIslands_BlueprintOf(Fixture.Graph);
	UNTEST_ASSERT_TRUE(IsValid(Blueprint));

	FStackRequest Request;
	FStackReport First;
	UNTEST_ASSERT_TRUE(Apply(Blueprint, Fixture.Graph, Request, First));
	UNTEST_ASSERT_EQ(First.NodesMoved, 4);

	UNTEST_EXPECT_TRUE(First.bRailStable);
	UNTEST_EXPECT_NEAR(First.RailNextDefault, First.RailX, 0.001);

	const TMap<FGuid, FIntPoint> AfterFirst = StackIslands_Snapshot(Fixture.Graph);

	FStackReport Second;
	UNTEST_ASSERT_TRUE(Apply(Blueprint, Fixture.Graph, Request, Second));
	UNTEST_EXPECT_EQ(Second.NodesMoved, 0);
	UNTEST_EXPECT_EQ(Second.IslandsPlaced.Num(), 2);
	UNTEST_EXPECT_NEAR(Second.RailX, First.RailX, 0.001);

	const TMap<FGuid, FIntPoint> AfterSecond = StackIslands_Snapshot(Fixture.Graph);
	bool bUnchanged = true;
	for (const TPair<FGuid, FIntPoint>& Pair : AfterFirst)
	{
		const FIntPoint* Now = AfterSecond.Find(Pair.Key);
		bUnchanged &= (Now != nullptr && *Now == Pair.Value);
	}
	UNTEST_EXPECT_TRUE(bUnchanged);
	co_return;
}

// Left-side feeders must not shift the rail on repeated calls; align entries instead of Box.MinX.
UNTEST_UNIT_OPTS(Claireon, StackIslands, FeederLeftOfEntryDoesNotDriftTheRail, UNTEST_TIMEOUTMS(30000))
{
	UEdGraph* Graph = StackIslands_MakeGraph();
	UNTEST_ASSERT_TRUE(IsValid(Graph));

	UK2Node_Event* EntryA = StackIslands_AddEvent(Graph, TEXT("DriftEventA"), 0, 0);
	UK2Node_CallFunction* CallA = StackIslands_AddCall(Graph, 300, 0);
	UK2Node_VariableGet* FeederA = StackIslands_AddFeeder(Graph, TEXT("DriftFeedA"), -500, 100);
	StackIslands_Connect(EntryA, CallA);
	StackIslands_Connect(FeederA, CallA);

	UK2Node_Event* EntryB = StackIslands_AddEvent(Graph, TEXT("DriftEventB"), 0, 1000);
	UK2Node_CallFunction* CallB = StackIslands_AddCall(Graph, 300, 1000);
	UK2Node_VariableGet* FeederB = StackIslands_AddFeeder(Graph, TEXT("DriftFeedB"), -500, 1100);
	StackIslands_Connect(EntryB, CallB);
	StackIslands_Connect(FeederB, CallB);

	const TArray<FIsland> Before = StackIslands_Islands(Graph);
	UNTEST_ASSERT_EQ(Before.Num(), 2);
	const FIsland* IslandA = StackIslands_IslandHolding(Before, EntryA);
	const FIsland* IslandB = StackIslands_IslandHolding(Before, EntryB);
	UNTEST_ASSERT_TRUE(IslandA != nullptr && IslandB != nullptr);
	UNTEST_ASSERT_EQ(IslandA->Nodes.Num(), 3);

	UNTEST_ASSERT_NEAR(IslandA->Box.MinX, -500.0, 0.001);
	UNTEST_ASSERT_NEAR(ClaireonGraphIslands::ResolveIslandEntryX(*IslandA), 0.0, 0.001);

	UBlueprint* Blueprint = StackIslands_BlueprintOf(Graph);
	UNTEST_ASSERT_TRUE(IsValid(Blueprint));

	FStackRequest Request;
	FStackReport First;
	UNTEST_ASSERT_TRUE(Apply(Blueprint, Graph, Request, First));
	UNTEST_EXPECT_NEAR(First.RailX, 0.0, 0.001);

	const FPlacedIsland* PlacedA = StackIslands_PlacedByRep(First, IslandA->Representative);
	const FPlacedIsland* PlacedB = StackIslands_PlacedByRep(First, IslandB->Representative);
	UNTEST_ASSERT_TRUE(PlacedA != nullptr && PlacedB != nullptr);
	UNTEST_EXPECT_EQ(PlacedA->DeltaX, 0);
	UNTEST_EXPECT_EQ(PlacedB->DeltaX, 0);
	UNTEST_EXPECT_EQ(EntryA->NodePosX, 0);
	UNTEST_EXPECT_EQ(EntryB->NodePosX, 0);
	UNTEST_EXPECT_NEAR(PlacedA->EntryXAfter, First.RailX, 0.001);
	UNTEST_EXPECT_NEAR(PlacedB->EntryXAfter, First.RailX, 0.001);
	UNTEST_EXPECT_TRUE(First.bRailStable);

	UNTEST_EXPECT_EQ(PlacedB->DeltaY, -388);
	UNTEST_EXPECT_EQ(First.NodesMoved, 3);

	FStackReport Second;
	UNTEST_ASSERT_TRUE(Apply(Blueprint, Graph, Request, Second));
	UNTEST_EXPECT_EQ(Second.NodesMoved, 0);
	UNTEST_EXPECT_NEAR(Second.RailX, First.RailX, 0.001);
	UNTEST_EXPECT_TRUE(Second.bRailStable);
	co_return;
}

#endif // WITH_UNTESTED
