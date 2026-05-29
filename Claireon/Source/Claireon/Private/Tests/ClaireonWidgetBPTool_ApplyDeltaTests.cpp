// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Functional tests for widgetbp_apply_delta (#0000).
// Verifies AR5/AR9 phase-1 rejection, M5 fail-on-missing, the happy path (remove +
// create + reparent), and the H4 reparent-delegation invariant (apply_delta phase 4
// must use the SAME helper as ClaireonWidgetBPTool_MoveWidget).

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonWidgetBPTool_ApplyDelta.h"
#include "Tools/ClaireonWidgetBPEditToolBase.h"
#include "Tools/FClaireonDeltaApplicator_WidgetBP.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/VerticalBox.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonWidgetBPTool_ApplyDeltaTests_anon
{
	static TSharedPtr<FJsonValue> WBPDeltaTest_ObjVal(const TSharedPtr<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }

	// Build a transient in-memory UWidgetBlueprint fixture with a CanvasPanel root and
	// a TextBlock child named "BodyText".
	static UWidgetBlueprint* WBPDeltaTest_CreateTransientWBP()
	{
		UPackage* Pkg = GetTransientPackage();
		UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
			Pkg, FName(TEXT("WBP_ClaireonDeltaFixture")), RF_Transient | RF_Transactional);
		if (!WBP) { return nullptr; }
		WBP->ParentClass = UUserWidget::StaticClass();
		WBP->WidgetTree = NewObject<UWidgetTree>(WBP, FName(TEXT("WidgetTree")), RF_Transactional);
		UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), FName(TEXT("Root")));
		WBP->WidgetTree->RootWidget = Root;
		UTextBlock* Body = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(TEXT("BodyText")));
		Root->AddChild(Body);
		return WBP;
	}

	static FString WBPDeltaTest_RegisterFakeSession(UWidgetBlueprint* WBP)
	{
		const FString SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
		FWidgetBPEditToolData NewData;
		NewData.WidgetBlueprint = WBP;
		ClaireonWidgetBPEditToolBase::ToolData.Add(SessionId, MoveTemp(NewData));
		return SessionId;
	}

	static void WBPDeltaTest_ClearFakeSession(const FString& SessionId)
	{
		ClaireonWidgetBPEditToolBase::ToolData.Remove(SessionId);
	}
}

// 1. Validation error: missing both session_id and asset_path.
UNTEST_UNIT_OPTS(Claireon, WidgetBPApplyDelta, MissingSessionAndAssetPath, UNTEST_TIMEOUTMS(10000))
{
	FClaireonWidgetBPTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("validate"));
	co_return;
}

// 2. M5 fail-on-missing: bogus asset_path returns an error.
UNTEST_UNIT_OPTS(Claireon, WidgetBPApplyDelta, FailOnMissingAsset, UNTEST_TIMEOUTMS(15000))
{
	FClaireonWidgetBPTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/__definitely_does_not_exist__WBP_zz"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

// 3. AR5/AR9: non-empty disconnect[] is rejected (widgetbp does not support phase 1).
UNTEST_UNIT_OPTS(Claireon, WidgetBPApplyDelta, RejectsPhase1NonEmpty, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonWidgetBPTool_ApplyDeltaTests_anon;
	FClaireonWidgetBPTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), TEXT("does-not-exist-session-id"));
	TArray<TSharedPtr<FJsonValue>> Disc;
	{
		TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetStringField(TEXT("any"), TEXT("thing"));
		Disc.Add(WBPDeltaTest_ObjVal(D));
	}
	Args->SetArrayField(TEXT("disconnect"), Disc);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("validate"));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("disconnect")));
	co_return;
}

// 4. Happy path: phase 2 removes BodyText; phase 3 creates a VerticalBox under Root,
//    and a Button under the VerticalBox (local-id resolution); phase 4 reparents the
//    Button back to Root via H4 MoveWidget helper.
UNTEST_UNIT_OPTS(Claireon, WidgetBPApplyDelta, HappyPath_RemoveCreateReparent, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonWidgetBPTool_ApplyDeltaTests_anon;
	UWidgetBlueprint* WBP = WBPDeltaTest_CreateTransientWBP();
	UNTEST_ASSERT_PTR(WBP);
	const FString SessionId = WBPDeltaTest_RegisterFakeSession(WBP);

	FClaireonWidgetBPTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	// Phase 2: remove BodyText (asset-resident named widget).
	TArray<TSharedPtr<FJsonValue>> Remove;
	Remove.Add(MakeShared<FJsonValueString>(TEXT("BodyText")));
	Args->SetArrayField(TEXT("remove_nodes"), Remove);

	// Phase 3: create VerticalBox under Root, then Button under local-id "vb".
	TArray<TSharedPtr<FJsonValue>> Nodes;
	{
		TSharedPtr<FJsonObject> N1 = MakeShared<FJsonObject>();
		N1->SetStringField(TEXT("id"), TEXT("vb"));
		N1->SetStringField(TEXT("type"), TEXT("VerticalBox"));
		N1->SetStringField(TEXT("parent_id"), TEXT("Root"));
		Nodes.Add(WBPDeltaTest_ObjVal(N1));

		TSharedPtr<FJsonObject> N2 = MakeShared<FJsonObject>();
		N2->SetStringField(TEXT("id"), TEXT("btn"));
		N2->SetStringField(TEXT("type"), TEXT("Button"));
		N2->SetStringField(TEXT("parent_id"), TEXT("vb"));  // local-id from N1
		Nodes.Add(WBPDeltaTest_ObjVal(N2));
	}
	Args->SetArrayField(TEXT("nodes"), Nodes);

	// Phase 4: reparent btn from vb back to Root.
	TArray<TSharedPtr<FJsonValue>> Conns;
	{
		TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("widget_id"), TEXT("btn"));
		C->SetStringField(TEXT("new_parent_id"), TEXT("Root"));
		Conns.Add(WBPDeltaTest_ObjVal(C));
	}
	Args->SetArrayField(TEXT("connections"), Conns);

	auto Result = Tool.Execute(Args);
	UNTEST_EXPECT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());
	int32 Removed = -1, Created = -1, ConnsMade = -1;
	Result.Data->TryGetNumberField(TEXT("removed_count"), Removed);
	Result.Data->TryGetNumberField(TEXT("created_count"), Created);
	Result.Data->TryGetNumberField(TEXT("connections_made"), ConnsMade);
	UNTEST_EXPECT_TRUE(Removed == 1);
	UNTEST_EXPECT_TRUE(Created == 2);
	UNTEST_EXPECT_TRUE(ConnsMade == 1);

	// Verify the actual tree state: Root has two children (vb + btn after reparent).
	UCanvasPanel* Root = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
	UNTEST_ASSERT_PTR(Root);
	UNTEST_EXPECT_TRUE(Root->GetChildrenCount() == 2);

	WBPDeltaTest_ClearFakeSession(SessionId);
	co_return;
}

// 5. M2 phase-3 rollback: invalid type token returns failed_phase: 3.
UNTEST_UNIT_OPTS(Claireon, WidgetBPApplyDelta, Phase3_InvalidType_RollsBack, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonWidgetBPTool_ApplyDeltaTests_anon;
	UWidgetBlueprint* WBP = WBPDeltaTest_CreateTransientWBP();
	UNTEST_ASSERT_PTR(WBP);
	const FString SessionId = WBPDeltaTest_RegisterFakeSession(WBP);

	FClaireonWidgetBPTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	{
		TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
		N->SetStringField(TEXT("id"), TEXT("bogus"));
		N->SetStringField(TEXT("type"), TEXT("ThisWidgetClassDoesNotExist__zz__"));
		N->SetStringField(TEXT("parent_id"), TEXT("Root"));
		Nodes.Add(WBPDeltaTest_ObjVal(N));
	}
	Args->SetArrayField(TEXT("nodes"), Nodes);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("3"));

	WBPDeltaTest_ClearFakeSession(SessionId);
	co_return;
}

// 6. H4 cross-validation: apply_delta phase 4 (reparent) and the shared MoveWidget helper
//    produce equivalent tree state. We run MoveWidget directly on one fixture and
//    apply_delta phase 4 on a second identical fixture, then check both trees match.
UNTEST_UNIT_OPTS(Claireon, WidgetBPApplyDelta, H4_ReparentMatchesMoveWidgetHelper, UNTEST_TIMEOUTMS(20000))
{
	using namespace ClaireonWidgetBPTool_ApplyDeltaTests_anon;

	// Both fixtures: Root (CanvasPanel) -> VerticalBox vb -> TextBlock t1.
	auto BuildFixture = []() -> UWidgetBlueprint*
	{
		UWidgetBlueprint* WBP = NewObject<UWidgetBlueprint>(
			GetTransientPackage(), NAME_None, RF_Transient | RF_Transactional);
		WBP->ParentClass = UUserWidget::StaticClass();
		WBP->WidgetTree = NewObject<UWidgetTree>(WBP, FName(TEXT("WidgetTree")), RF_Transactional);
		UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), FName(TEXT("Root")));
		WBP->WidgetTree->RootWidget = Root;
		UVerticalBox* VB = WBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), FName(TEXT("vb")));
		Root->AddChild(VB);
		UTextBlock* T = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), FName(TEXT("t1")));
		VB->AddChild(T);
		return WBP;
	};

	UWidgetBlueprint* WbpA = BuildFixture();
	UWidgetBlueprint* WbpB = BuildFixture();
	UNTEST_ASSERT_PTR(WbpA);
	UNTEST_ASSERT_PTR(WbpB);

	// Path A: call the shared helper directly.
	{
		UWidget* T = ClaireonWidgetHelpers::FindWidgetByName(WbpA->WidgetTree, FName(TEXT("t1")));
		UWidget* Root = ClaireonWidgetHelpers::FindWidgetByName(WbpA->WidgetTree, FName(TEXT("Root")));
		FString Err;
		UNTEST_ASSERT_TRUE(ClaireonWidgetHelpers::MoveWidget(WbpA, T, Root, -1, Err));
	}

	// Path B: apply_delta phase 4 reparent.
	{
		const FString SessionB = WBPDeltaTest_RegisterFakeSession(WbpB);
		FClaireonWidgetBPTool_ApplyDelta Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionB);
		TArray<TSharedPtr<FJsonValue>> Conns;
		TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("widget_id"), TEXT("t1"));
		C->SetStringField(TEXT("new_parent_id"), TEXT("Root"));
		Conns.Add(WBPDeltaTest_ObjVal(C));
		Args->SetArrayField(TEXT("connections"), Conns);
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_FALSE(Result.bIsError);
		WBPDeltaTest_ClearFakeSession(SessionB);
	}

	// Both fixtures: Root now contains BOTH vb and t1; vb is empty.
	auto CheckTree = [](UWidgetBlueprint* WBP) -> bool
	{
		UCanvasPanel* Root = Cast<UCanvasPanel>(WBP->WidgetTree->RootWidget);
		if (!Root) { return false; }
		if (Root->GetChildrenCount() != 2) { return false; }
		UVerticalBox* VB = Cast<UVerticalBox>(ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(TEXT("vb"))));
		if (!VB) { return false; }
		if (VB->GetChildrenCount() != 0) { return false; }
		return true;
	};
	UNTEST_EXPECT_TRUE(CheckTree(WbpA));
	UNTEST_EXPECT_TRUE(CheckTree(WbpB));

	co_return;
}

#endif // WITH_UNTESTED
