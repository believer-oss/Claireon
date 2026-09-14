// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Test custom-event name conflicts in the authoring guard and lint.
// Use Actor ReceiveBeginPlay to exercise parent-event ownership without UMG.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonBlueprintNodeFactory.h"
#include "ClaireonLintTypes.h"

#include "Dom/JsonObject.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace ClaireonCustomEventConflictTestsNS
{
	// Prefix helpers to avoid unity-build collisions.

	using FConflict = ClaireonBlueprintHelpers::FCustomEventNameConflict;

	/** An unsaved Actor Blueprint under /Game/__MCPTests/. */
	UBlueprint* EventConflict_CreateActorBP(const TCHAR* Name)
	{
		const FString AssetPath = FString(TEXT("/Game/__MCPTests/")) + Name;
		UPackage* Package = CreatePackage(*AssetPath);
		if (!IsValid(Package))
		{
			return nullptr;
		}
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Package,
			FName(Name),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);

		// Remove template events so parent-class detection cannot pass through the existing-node branch.
		if (IsValid(BP))
		{
			for (UEdGraph* Page : BP->UbergraphPages)
			{
				if (!IsValid(Page))
				{
					continue;
				}
				for (int32 I = Page->Nodes.Num() - 1; I >= 0; --I)
				{
					if (Page->Nodes[I] && Page->Nodes[I]->IsA<UK2Node_Event>())
					{
						Page->Nodes.RemoveAt(I);
					}
				}
			}
		}
		return BP;
	}

	UEdGraph* EventConflict_FirstUbergraph(UBlueprint* BP)
	{
		return (IsValid(BP) && BP->UbergraphPages.Num() > 0) ? BP->UbergraphPages[0] : nullptr;
	}

	/** Bypass the factory guard to build a conflict for lint. */
	UK2Node_CustomEvent* EventConflict_AddRawCustomEvent(UEdGraph* Graph, const TCHAR* EventName)
	{
		if (!IsValid(Graph))
		{
			return nullptr;
		}
		UK2Node_CustomEvent* Node = NewObject<UK2Node_CustomEvent>(Graph);
		Node->CustomFunctionName = FName(EventName);
		Node->CreateNewGuid();
		Graph->AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	/** Add the real override event node for a parent-class event. */
	UK2Node_Event* EventConflict_AddOverrideEvent(UBlueprint* BP, UEdGraph* Graph, const TCHAR* FunctionName)
	{
		if (!IsValid(BP) || !IsValid(Graph))
		{
			return nullptr;
		}
		UK2Node_Event* Node = NewObject<UK2Node_Event>(Graph);
		Node->EventReference.SetExternalMember(FName(FunctionName), BP->ParentClass);
		Node->bOverrideFunction = true;
		Node->CreateNewGuid();
		Graph->AddNode(Node, /*bFromUI=*/false, /*bSelectNewNode=*/false);
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		return Node;
	}

	TSharedPtr<FJsonObject> EventConflict_CustomEventParams(const TCHAR* EventName)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_type"), TEXT("CustomEvent"));
		Params->SetStringField(TEXT("event_name"), EventName);
		return Params;
	}
}

using namespace ClaireonCustomEventConflictTestsNS;


UNTEST_UNIT_OPTS(Claireon, CustomEventConflict, Detect_ParentBlueprintEvent, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* BP = EventConflict_CreateActorBP(TEXT("BP_EventConflict_ParentEvent"));
	UNTEST_ASSERT_PTR(BP);

	const FConflict Exact = ClaireonBlueprintHelpers::FindCustomEventNameConflict(BP, TEXT("ReceiveBeginPlay"));
	UNTEST_ASSERT_TRUE(Exact.IsConflict());
	UNTEST_EXPECT_TRUE(Exact.Kind == FConflict::EKind::ParentBlueprintEvent);
	UNTEST_EXPECT_TRUE(Exact.ResolvedFunctionName == FName(TEXT("ReceiveBeginPlay")));
	UNTEST_EXPECT_FALSE(Exact.bNativeEvent);
	UNTEST_EXPECT_TRUE(Exact.Remedy.Contains(TEXT("EventOverride")));

	// Friendly aliases must resolve to the same parent event.
	const FConflict Aliased = ClaireonBlueprintHelpers::FindCustomEventNameConflict(BP, TEXT("BeginPlay"));
	UNTEST_ASSERT_TRUE(Aliased.IsConflict());
	UNTEST_EXPECT_TRUE(Aliased.Kind == FConflict::EKind::ParentBlueprintEvent);
	UNTEST_EXPECT_TRUE(Aliased.ResolvedFunctionName == FName(TEXT("ReceiveBeginPlay")));
	UNTEST_EXPECT_FALSE(Aliased.ResolutionNote.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CustomEventConflict, Detect_NovelNameIsFree, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* BP = EventConflict_CreateActorBP(TEXT("BP_EventConflict_Novel"));
	UNTEST_ASSERT_PTR(BP);

	const FConflict None = ClaireonBlueprintHelpers::FindCustomEventNameConflict(
		BP, TEXT("EventConflict_HandleThrowSlotReady"));
	UNTEST_EXPECT_FALSE(None.IsConflict());
	UNTEST_EXPECT_TRUE(None.Kind == FConflict::EKind::None);

	// Near-miss names must not match by substring.
	const FConflict NearMiss = ClaireonBlueprintHelpers::FindCustomEventNameConflict(BP, TEXT("BeginPlaySequence"));
	UNTEST_EXPECT_FALSE(NearMiss.IsConflict());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CustomEventConflict, Detect_ExistingOverrideEvent, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* BP = EventConflict_CreateActorBP(TEXT("BP_EventConflict_ExistingOverride"));
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* Graph = EventConflict_FirstUbergraph(BP);
	UNTEST_ASSERT_PTR(Graph);

	UK2Node_Event* Override = EventConflict_AddOverrideEvent(BP, Graph, TEXT("ReceiveBeginPlay"));
	UNTEST_ASSERT_PTR(Override);

	const FConflict Conflict = ClaireonBlueprintHelpers::FindCustomEventNameConflict(BP, TEXT("ReceiveBeginPlay"));
	UNTEST_ASSERT_TRUE(Conflict.IsConflict());
	// Prefer the existing node and its GUID over the parent-class conflict.
	UNTEST_EXPECT_TRUE(Conflict.Kind == FConflict::EKind::ExistingOverrideEvent);
	UNTEST_EXPECT_TRUE(Conflict.ExistingNodeGuid ==
		Override->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	UNTEST_EXPECT_TRUE(Conflict.Remedy.Contains(Conflict.ExistingNodeGuid));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CustomEventConflict, Detect_ExistingCustomEventAndSelfExclusion, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* BP = EventConflict_CreateActorBP(TEXT("BP_EventConflict_Duplicate"));
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* Graph = EventConflict_FirstUbergraph(BP);
	UNTEST_ASSERT_PTR(Graph);

	UK2Node_CustomEvent* First = EventConflict_AddRawCustomEvent(Graph, TEXT("EventConflict_Handler"));
	UNTEST_ASSERT_PTR(First);

	const FConflict Conflict = ClaireonBlueprintHelpers::FindCustomEventNameConflict(
		BP, TEXT("EventConflict_Handler"));
	UNTEST_ASSERT_TRUE(Conflict.IsConflict());
	UNTEST_EXPECT_TRUE(Conflict.Kind == FConflict::EKind::ExistingCustomEvent);

	// IgnoreNode prevents lint from reporting an event as its own duplicate.
	const FConflict SelfIgnored = ClaireonBlueprintHelpers::FindCustomEventNameConflict(
		BP, TEXT("EventConflict_Handler"), First);
	UNTEST_EXPECT_FALSE(SelfIgnored.IsConflict());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CustomEventConflict, Detect_ParentFunctionAndLocalGraph, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* BP = EventConflict_CreateActorBP(TEXT("BP_EventConflict_Functions"));
	UNTEST_ASSERT_PTR(BP);

	const FConflict ParentFn = ClaireonBlueprintHelpers::FindCustomEventNameConflict(
		BP, TEXT("SetActorHiddenInGame"));
	UNTEST_ASSERT_TRUE(ParentFn.IsConflict());
	UNTEST_EXPECT_TRUE(ParentFn.Kind == FConflict::EKind::ParentFunction);
	UNTEST_EXPECT_TRUE(ParentFn.Remedy.Contains(TEXT("CallFunction")));

	UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(TEXT("EventConflict_LocalFunc")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	UNTEST_ASSERT_PTR(FuncGraph);
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, FuncGraph, /*bIsUserCreated=*/true, /*SignatureFromClass=*/nullptr);

	const FConflict LocalFn = ClaireonBlueprintHelpers::FindCustomEventNameConflict(
		BP, TEXT("EventConflict_LocalFunc"));
	UNTEST_ASSERT_TRUE(LocalFn.IsConflict());
	UNTEST_EXPECT_TRUE(LocalFn.Kind == FConflict::EKind::LocalFunctionGraph);
	co_return;
}

// The shared factory guard covers every authoring path using it.

UNTEST_UNIT_OPTS(Claireon, CustomEventConflict, Factory_RefusesShadowingCustomEvent, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* BP = EventConflict_CreateActorBP(TEXT("BP_EventConflict_FactoryRefuse"));
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* Graph = EventConflict_FirstUbergraph(BP);
	UNTEST_ASSERT_PTR(Graph);

	const int32 NodesBefore = Graph->Nodes.Num();

	ClaireonBlueprintNodeFactory::FCreateResult R = ClaireonBlueprintNodeFactory::CreateNode(
		BP, Graph, EventConflict_CustomEventParams(TEXT("BeginPlay")), FVector2D(0.0, 0.0));

	UNTEST_EXPECT_FALSE(R.IsOk());
	UNTEST_EXPECT_TRUE(R.Node == nullptr);
	UNTEST_EXPECT_TRUE(R.Error.Contains(TEXT("EventOverride")));
	UNTEST_EXPECT_TRUE(R.Error.Contains(TEXT("ReceiveBeginPlay")));

	UNTEST_EXPECT_EQ(Graph->Nodes.Num(), NodesBefore);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CustomEventConflict, Factory_AllowsNovelCustomEvent, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* BP = EventConflict_CreateActorBP(TEXT("BP_EventConflict_FactoryAllow"));
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* Graph = EventConflict_FirstUbergraph(BP);
	UNTEST_ASSERT_PTR(Graph);

	ClaireonBlueprintNodeFactory::FCreateResult R = ClaireonBlueprintNodeFactory::CreateNode(
		BP, Graph, EventConflict_CustomEventParams(TEXT("EventConflict_OnSlotReady")), FVector2D(0.0, 0.0));

	UNTEST_ASSERT_TRUE(R.IsOk());
	UK2Node_CustomEvent* Created = Cast<UK2Node_CustomEvent>(R.Node);
	UNTEST_ASSERT_PTR(Created);
	UNTEST_EXPECT_TRUE(Created->CustomFunctionName == FName(TEXT("EventConflict_OnSlotReady")));

	ClaireonBlueprintNodeFactory::FCreateResult Second = ClaireonBlueprintNodeFactory::CreateNode(
		BP, Graph, EventConflict_CustomEventParams(TEXT("EventConflict_OnSlotReady")), FVector2D(0.0, 0.0));
	UNTEST_EXPECT_FALSE(Second.IsOk());
	UNTEST_EXPECT_TRUE(Second.Error.Contains(TEXT("already exists")));
	co_return;
}

// Lint detects conflicts already present in content.

UNTEST_UNIT_OPTS(Claireon, CustomEventConflict, Lint_ReportsShadowingCustomEvent, UNTEST_TIMEOUTMS(60000))
{
	UBlueprint* BP = EventConflict_CreateActorBP(TEXT("BP_EventConflict_Lint"));
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* Graph = EventConflict_FirstUbergraph(BP);
	UNTEST_ASSERT_PTR(Graph);

	UNTEST_ASSERT_PTR(EventConflict_AddOverrideEvent(BP, Graph, TEXT("ReceiveBeginPlay")));
	UNTEST_ASSERT_PTR(EventConflict_AddRawCustomEvent(Graph, TEXT("ReceiveBeginPlay")));
	// Include an unrelated custom event as a negative control.
	UNTEST_ASSERT_PTR(EventConflict_AddRawCustomEvent(Graph, TEXT("EventConflict_Innocent")));

	FClaireonLintContext Context;
	Context.Graph = Graph;
	Context.GraphName = Graph->GetName();
	Context.Blueprint = BP;

	TArray<FClaireonLintFinding> Findings;
	ClaireonLint::RunHygieneRules(Context, Findings);

	int32 ShadowFindings = 0;
	for (const FClaireonLintFinding& Finding : Findings)
	{
		if (Finding.Rule != TEXT("custom-event-shadows-function"))
		{
			continue;
		}
		++ShadowFindings;
		UNTEST_EXPECT_TRUE(Finding.Severity == EClaireonLintSeverity::Warning);
		UNTEST_EXPECT_TRUE(Finding.Confidence == EClaireonLintConfidence::High);
		UNTEST_EXPECT_TRUE(Finding.Evidence.IsValid());

		FString EventName;
		UNTEST_EXPECT_TRUE(Finding.Evidence->TryGetStringField(TEXT("event_name"), EventName));
		UNTEST_EXPECT_TRUE(EventName == TEXT("ReceiveBeginPlay"));

		FString ConflictKind;
		UNTEST_EXPECT_TRUE(Finding.Evidence->TryGetStringField(TEXT("conflict"), ConflictKind));
		UNTEST_EXPECT_TRUE(ConflictKind == TEXT("existing_override_event"));

		FString Remedy;
		UNTEST_EXPECT_TRUE(Finding.Evidence->TryGetStringField(TEXT("remedy"), Remedy));
		UNTEST_EXPECT_FALSE(Remedy.IsEmpty());
	}

	UNTEST_EXPECT_EQ(ShadowFindings, 1);
	co_return;
}

#endif // WITH_UNTESTED
