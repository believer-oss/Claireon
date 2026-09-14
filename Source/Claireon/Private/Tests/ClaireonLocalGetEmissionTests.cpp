// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test local parameter getters and copies of distant variable getters.
// Copied references must preserve member/local scope and receiver semantics.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLintTypes.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_VariableGet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/Package.h"

namespace ClaireonLocalGetEmissionTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	static const TCHAR* LocalGet_ParamName = TEXT("LocalGetTestParam");
	static const TCHAR* LocalGet_MemberName = TEXT("LocalGetTestMember");
	static const TCHAR* LocalGet_FuncName = TEXT("LocalGetTestFunc");

	UBlueprint* LocalGet_MakeBP(const TCHAR* Name)
	{
		UPackage* Package = CreatePackage(*(FString(TEXT("/Game/__MCPTests/")) + Name));
		if (!IsValid(Package))
		{
			return nullptr;
		}
		return FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(), Package, FName(Name), BPTYPE_Normal,
			UBlueprint::StaticClass(), UBlueprintGeneratedClass::StaticClass(), NAME_None);
	}

	/** A function graph carrying one bool input parameter, plus a bool member variable on
	 *  the Blueprint so the member-versus-local distinction can be exercised. */
	UEdGraph* LocalGet_MakeFunctionGraph(UBlueprint* BP)
	{
		if (!IsValid(BP))
		{
			return nullptr;
		}

		FEdGraphPinType BoolType;
		BoolType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		FBlueprintEditorUtils::AddMemberVariable(BP, FName(LocalGet_MemberName), BoolType);

		UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FName(LocalGet_FuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!IsValid(FuncGraph))
		{
			return nullptr;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, FuncGraph, /*bIsUserCreated=*/true, /*SignatureFromClass=*/nullptr);

		TArray<UK2Node_FunctionEntry*> EntryNodes;
		FuncGraph->GetNodesOfClass<UK2Node_FunctionEntry>(EntryNodes);
		if (EntryNodes.Num() == 0)
		{
			return nullptr;
		}
		FEdGraphPinType ParamType;
		ParamType.PinCategory = UEdGraphSchema_K2::PC_Boolean;
		EntryNodes[0]->CreateUserDefinedPin(FName(LocalGet_ParamName), ParamType, EGPD_Output);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		return FuncGraph;
	}

	UK2Node_CallFunction* LocalGet_AddConsumer(UEdGraph* Graph, int32 X, int32 Y)
	{
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(Graph);
		Graph->AddNode(Call, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Call->FunctionReference.SetExternalMember(
			FName(TEXT("PrintString")), UKismetSystemLibrary::StaticClass());
		Call->CreateNewGuid();
		Call->NodePosX = X;
		Call->NodePosY = Y;
		Call->PostPlacedNewNode();
		Call->AllocateDefaultPins();
		return Call;
	}

	UK2Node_VariableGet* LocalGet_AddMemberGet(UEdGraph* Graph, int32 X, int32 Y)
	{
		UK2Node_VariableGet* Get = NewObject<UK2Node_VariableGet>(Graph);
		Graph->AddNode(Get, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Get->VariableReference.SetSelfMember(FName(LocalGet_MemberName));
		Get->CreateNewGuid();
		Get->NodePosX = X;
		Get->NodePosY = Y;
		Get->PostPlacedNewNode();
		Get->AllocateDefaultPins();
		return Get;
	}

	UEdGraphPin* LocalGet_FirstDataPin(UEdGraphNode* Node, EEdGraphPinDirection Direction)
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
}

using namespace ClaireonLocalGetEmissionTestsNS;

// Keep authoring and lint distance thresholds aligned.
UNTEST_UNIT(Claireon, LocalGetEmission, Threshold_AuthoringMatchesLint)
{
	const FClaireonLintThresholds Defaults;
	UNTEST_EXPECT_NEAR(ClaireonBlueprintHelpers::LocalGetDistanceUnits, Defaults.LocalGetDistance, 0.001);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LocalGetEmission, ParameterGet_IsLocalScopeAndAdjacent, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* BP = LocalGet_MakeBP(TEXT("BP_LocalGet_Param"));
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* FuncGraph = LocalGet_MakeFunctionGraph(BP);
	UNTEST_ASSERT_PTR(FuncGraph);

	TArray<UK2Node_FunctionEntry*> EntryNodes;
	FuncGraph->GetNodesOfClass<UK2Node_FunctionEntry>(EntryNodes);
	UNTEST_ASSERT_TRUE(EntryNodes.Num() > 0);
	UEdGraphPin* ParamPin = EntryNodes[0]->FindPin(FName(LocalGet_ParamName));
	UNTEST_ASSERT_TRUE(ParamPin != nullptr);
	UNTEST_EXPECT_TRUE(ClaireonBlueprintHelpers::IsFunctionParameterSource(ParamPin));

	UK2Node_CallFunction* Consumer = LocalGet_AddConsumer(FuncGraph, 4000, 800);
	UEdGraphPin* TargetPin = LocalGet_FirstDataPin(Consumer, EGPD_Input);
	UNTEST_ASSERT_TRUE(TargetPin != nullptr);

	UEdGraphPin* Emitted = ClaireonBlueprintHelpers::EmitLocalParameterGet(
		FuncGraph, ParamPin, TargetPin, /*StackIndex=*/0);
	UNTEST_ASSERT_TRUE(Emitted != nullptr);

	UK2Node_VariableGet* Get = Cast<UK2Node_VariableGet>(Emitted->GetOwningNode());
	UNTEST_ASSERT_PTR(Get);
	UNTEST_EXPECT_TRUE(Get->VariableReference.GetMemberName() == FName(LocalGet_ParamName));
	UNTEST_EXPECT_TRUE(Get->VariableReference.IsLocalScope());
	UNTEST_EXPECT_FALSE(Get->VariableReference.IsSelfContext());
	UNTEST_EXPECT_TRUE(Get->VariableReference.GetMemberScopeName() == FuncGraph->GetName());

	UNTEST_EXPECT_EQ(Get->NodePosX, Consumer->NodePosX - 240);
	UNTEST_EXPECT_EQ(Get->NodePosY, Consumer->NodePosY);

	UEdGraphPin* Second = ClaireonBlueprintHelpers::EmitLocalParameterGet(
		FuncGraph, ParamPin, TargetPin, /*StackIndex=*/1);
	UNTEST_ASSERT_TRUE(Second != nullptr);
	UNTEST_EXPECT_TRUE(Second->GetOwningNode()->NodePosY != Get->NodePosY);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LocalGetEmission, DistantSource_ThresholdBoundary, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* BP = LocalGet_MakeBP(TEXT("BP_LocalGet_Boundary"));
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* Graph = LocalGet_MakeFunctionGraph(BP);
	UNTEST_ASSERT_PTR(Graph);

	UK2Node_VariableGet* Get = LocalGet_AddMemberGet(Graph, 0, 0);
	UEdGraphPin* SourcePin = LocalGet_FirstDataPin(Get, EGPD_Output);
	UNTEST_ASSERT_TRUE(SourcePin != nullptr);

	UK2Node_CallFunction* Near = LocalGet_AddConsumer(Graph, 512, 0);
	UK2Node_CallFunction* Far = LocalGet_AddConsumer(Graph, 513, 0);
	UEdGraphPin* NearPin = LocalGet_FirstDataPin(Near, EGPD_Input);
	UEdGraphPin* FarPin = LocalGet_FirstDataPin(Far, EGPD_Input);
	UNTEST_ASSERT_TRUE(NearPin != nullptr && FarPin != nullptr);

	const double Threshold = ClaireonBlueprintHelpers::LocalGetDistanceUnits;
	UNTEST_EXPECT_FALSE(ClaireonBlueprintHelpers::IsDistantVariableGetSource(SourcePin, NearPin, Threshold));
	UNTEST_EXPECT_TRUE(ClaireonBlueprintHelpers::IsDistantVariableGetSource(SourcePin, FarPin, Threshold));

	for (UEdGraphPin* Pin : Get->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
		{
			UNTEST_EXPECT_FALSE(ClaireonBlueprintHelpers::IsDistantVariableGetSource(Pin, FarPin, Threshold));
		}
	}
	UEdGraphPin* CallOutput = LocalGet_FirstDataPin(Far, EGPD_Output);
	if (CallOutput)
	{
		UNTEST_EXPECT_FALSE(ClaireonBlueprintHelpers::IsDistantVariableGetSource(CallOutput, NearPin, Threshold));
	}
	co_return;
}

// Preserve the whole member reference when a local could shadow its name.
UNTEST_UNIT_OPTS(Claireon, LocalGetEmission, AdjacentGet_CopiesReferenceVerbatim, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* BP = LocalGet_MakeBP(TEXT("BP_LocalGet_Copy"));
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* Graph = LocalGet_MakeFunctionGraph(BP);
	UNTEST_ASSERT_PTR(Graph);

	UK2Node_VariableGet* MemberGet = LocalGet_AddMemberGet(Graph, 0, 0);
	UEdGraphPin* SourcePin = LocalGet_FirstDataPin(MemberGet, EGPD_Output);
	UNTEST_ASSERT_TRUE(SourcePin != nullptr);

	UK2Node_CallFunction* Consumer = LocalGet_AddConsumer(Graph, 3000, 200);
	UEdGraphPin* TargetPin = LocalGet_FirstDataPin(Consumer, EGPD_Input);
	UNTEST_ASSERT_TRUE(TargetPin != nullptr);

	UEdGraphPin* Emitted = ClaireonBlueprintHelpers::EmitAdjacentVariableGet(
		Graph, SourcePin, TargetPin, /*StackIndex=*/0);
	UNTEST_ASSERT_TRUE(Emitted != nullptr);

	UK2Node_VariableGet* Copy = Cast<UK2Node_VariableGet>(Emitted->GetOwningNode());
	UNTEST_ASSERT_PTR(Copy);
	UNTEST_EXPECT_TRUE(Copy->VariableReference.GetMemberName() == FName(LocalGet_MemberName));
	UNTEST_EXPECT_TRUE(Copy->VariableReference.IsSelfContext());
	UNTEST_EXPECT_FALSE(Copy->VariableReference.IsLocalScope());
	UNTEST_EXPECT_TRUE(Emitted->PinName == SourcePin->PinName);
	// Keep the original getter for other consumers.
	UNTEST_EXPECT_EQ(Copy->NodePosX, Consumer->NodePosX - 240);
	UNTEST_EXPECT_EQ(MemberGet->NodePosX, 0);
	co_return;
}

// Only pure, receiver-less reads may be copied; reject explicit targets and validated getters.
UNTEST_UNIT_OPTS(Claireon, LocalGetEmission, DistantSource_RejectsExplicitReceiverAndImpureGets, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* BP = LocalGet_MakeBP(TEXT("BP_LocalGet_Receiver"));
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* Graph = LocalGet_MakeFunctionGraph(BP);
	UNTEST_ASSERT_PTR(Graph);

	UK2Node_VariableGet* Get = LocalGet_AddMemberGet(Graph, 0, 0);
	UEdGraphPin* SourcePin = LocalGet_FirstDataPin(Get, EGPD_Output);
	UNTEST_ASSERT_TRUE(SourcePin != nullptr);

	UK2Node_CallFunction* Far = LocalGet_AddConsumer(Graph, 3000, 0);
	UEdGraphPin* FarPin = LocalGet_FirstDataPin(Far, EGPD_Input);
	UNTEST_ASSERT_TRUE(FarPin != nullptr);

	const double Threshold = ClaireonBlueprintHelpers::LocalGetDistanceUnits;

	UNTEST_EXPECT_TRUE(ClaireonBlueprintHelpers::IsDistantVariableGetSource(SourcePin, FarPin, Threshold));

	UEdGraphPin* SelfPin = Get->FindPin(UEdGraphSchema_K2::PN_Self, EGPD_Input);
	UNTEST_ASSERT_TRUE(SelfPin != nullptr);

	UK2Node_VariableGet* Receiver = LocalGet_AddMemberGet(Graph, -400, 0);
	UEdGraphPin* ReceiverOut = LocalGet_FirstDataPin(Receiver, EGPD_Output);
	UNTEST_ASSERT_TRUE(ReceiverOut != nullptr);
	SelfPin->MakeLinkTo(ReceiverOut);
	UNTEST_EXPECT_FALSE(ClaireonBlueprintHelpers::IsDistantVariableGetSource(SourcePin, FarPin, Threshold));
	SelfPin->BreakAllPinLinks();

	SelfPin->DefaultObject = BP;
	UNTEST_EXPECT_FALSE(ClaireonBlueprintHelpers::IsDistantVariableGetSource(SourcePin, FarPin, Threshold));
	SelfPin->DefaultObject = nullptr;

	// Restore the bare shape as a positive control.
	UNTEST_EXPECT_TRUE(ClaireonBlueprintHelpers::IsDistantVariableGetSource(SourcePin, FarPin, Threshold));

	// Validated getters carry exec semantics a pure copy cannot preserve.
	Get->CreatePin(EGPD_Input, UEdGraphSchema_K2::PC_Exec, UEdGraphSchema_K2::PN_Execute);
	UNTEST_EXPECT_FALSE(ClaireonBlueprintHelpers::IsDistantVariableGetSource(SourcePin, FarPin, Threshold));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LocalGetEmission, AdjacentGet_KeepsLocalScope, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* BP = LocalGet_MakeBP(TEXT("BP_LocalGet_KeepsLocal"));
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* Graph = LocalGet_MakeFunctionGraph(BP);
	UNTEST_ASSERT_PTR(Graph);

	UK2Node_VariableGet* LocalGetNode = NewObject<UK2Node_VariableGet>(Graph);
	Graph->AddNode(LocalGetNode, /*bFromUI=*/false, /*bSelectNewNode=*/false);
	LocalGetNode->VariableReference.SetLocalMember(FName(LocalGet_ParamName), Graph->GetName(), FGuid());
	LocalGetNode->CreateNewGuid();
	LocalGetNode->PostPlacedNewNode();
	LocalGetNode->AllocateDefaultPins();
	if (LocalGetNode->Pins.Num() == 0)
	{
		LocalGetNode->CreatePin(EGPD_Output, UEdGraphSchema_K2::PC_Boolean, FName(LocalGet_ParamName));
	}

	UEdGraphPin* SourcePin = LocalGet_FirstDataPin(LocalGetNode, EGPD_Output);
	UNTEST_ASSERT_TRUE(SourcePin != nullptr);
	UK2Node_CallFunction* Consumer = LocalGet_AddConsumer(Graph, 2500, 0);
	UEdGraphPin* TargetPin = LocalGet_FirstDataPin(Consumer, EGPD_Input);
	UNTEST_ASSERT_TRUE(TargetPin != nullptr);

	UEdGraphPin* Emitted = ClaireonBlueprintHelpers::EmitAdjacentVariableGet(
		Graph, SourcePin, TargetPin, /*StackIndex=*/0);
	UNTEST_ASSERT_TRUE(Emitted != nullptr);

	UK2Node_VariableGet* Copy = Cast<UK2Node_VariableGet>(Emitted->GetOwningNode());
	UNTEST_ASSERT_PTR(Copy);
	UNTEST_EXPECT_TRUE(Copy->VariableReference.IsLocalScope());
	UNTEST_EXPECT_TRUE(Copy->VariableReference.GetMemberScopeName() == Graph->GetName());
	UNTEST_EXPECT_TRUE(Copy->VariableReference.GetMemberName() == FName(LocalGet_ParamName));
	co_return;
}

#endif // WITH_UNTESTED
