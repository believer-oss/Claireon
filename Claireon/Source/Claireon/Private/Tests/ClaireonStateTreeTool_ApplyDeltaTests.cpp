// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Functional tests for statetree_apply_delta (#0000).
// Verifies AR4 dual-shape transitions, dedupe rules (M3), and base-class invariants.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonStateTreeTool_ApplyDelta.h"
#include "Tools/ClaireonStateTreeTool_Open.h"
#include "Tools/ClaireonStateTreeTool_Close.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "Tools/ClaireonStateTreeEditToolBase.h"
#include "Tools/FClaireonDeltaApplicator_StateTree.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"

namespace ClaireonStateTreeTool_ApplyDeltaTests_anon
{
	static const TCHAR* STDeltaTestAssetPath = TEXT("/Game/BP/AI/ST/ST_TestDummy");

	static TSharedPtr<FJsonValue> STDeltaTest_ObjVal(const TSharedPtr<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }
	static TSharedPtr<FJsonValue> STDeltaTest_StrVal(const FString& V) { return MakeShared<FJsonValueString>(V); }

	static FString STDeltaTest_OpenSession()
	{
		ClaireonStateTreeTool_Open OpenTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), STDeltaTestAssetPath);
		auto Result = OpenTool.Execute(Args);
		if (Result.bIsError || !Result.Data.IsValid()) { return FString(); }
		FString SessionId;
		Result.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	static void STDeltaTest_CloseSession(const FString& SessionId)
	{
		if (SessionId.IsEmpty()) { return; }
		ClaireonStateTreeTool_Close CloseTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		CloseTool.Execute(Args);
	}

	static UStateTreeEditorData* STDeltaTest_GetEditorData(const FString& SessionId)
	{
		FStateTreeEditToolData* Data = ClaireonStateTreeEditToolBase::ToolData.Find(SessionId);
		if (!Data || !Data->IsValid()) { return nullptr; }
		FString Err;
		return ClaireonStateTreeHelpers::GetEditorData(Data->StateTree.Get(), Err);
	}

	static int32 STDeltaTest_CountSubTrees(UStateTreeEditorData* ED)
	{
		return ED ? ED->SubTrees.Num() : -1;
	}

	static int32 STDeltaTest_CountAllTransitions(UStateTreeEditorData* ED)
	{
		if (!ED) { return -1; }
		int32 Total = 0;
		TArray<UStateTreeState*> Stack;
		for (UStateTreeState* Root : ED->SubTrees) { if (Root) { Stack.Add(Root); } }
		while (Stack.Num() > 0)
		{
			UStateTreeState* S = Stack.Pop(EAllowShrinking::No);
			if (!S) { continue; }
			Total += S->Transitions.Num();
			for (UStateTreeState* Child : S->Children) { if (Child) { Stack.Add(Child); } }
		}
		return Total;
	}
}

// 1. Validation error: missing both session_id and asset_path.
UNTEST_UNIT_OPTS(Claireon, StateTreeApplyDelta, MissingSessionAndAssetPath, UNTEST_TIMEOUTMS(10000))
{
	FClaireonStateTreeTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("validate"));
	co_return;
}

// 2. Rejection: 'transition' kind in remove_nodes[] -> validation error.
UNTEST_UNIT_OPTS(Claireon, StateTreeApplyDelta, RejectTransitionKindInRemove, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonStateTreeTool_ApplyDeltaTests_anon;
	const FString SessionId = STDeltaTest_OpenSession();
	if (SessionId.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[StateTreeApplyDelta] Test asset not present; skipping."));
		co_return;
	}

	FClaireonStateTreeTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	TArray<TSharedPtr<FJsonValue>> Remove;
	{
		TSharedPtr<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("kind"), TEXT("transition"));
		R->SetStringField(TEXT("id"), TEXT("00000000-0000-0000-0000-000000000001"));
		Remove.Add(STDeltaTest_ObjVal(R));
	}
	Args->SetArrayField(TEXT("remove_nodes"), Remove);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("validate"));

	STDeltaTest_CloseSession(SessionId);
	co_return;
}

// 3. Happy path: create two root states via phase 3; id_map populated; subtree count grows by 2.
UNTEST_UNIT_OPTS(Claireon, StateTreeApplyDelta, CreateRootStatesPhase3, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonStateTreeTool_ApplyDeltaTests_anon;
	const FString SessionId = STDeltaTest_OpenSession();
	if (SessionId.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[StateTreeApplyDelta] Test asset not present; skipping."));
		co_return;
	}
	UStateTreeEditorData* ED = STDeltaTest_GetEditorData(SessionId);
	UNTEST_ASSERT_PTR(ED);

	const int32 BeforeCount = STDeltaTest_CountSubTrees(ED);

	FClaireonStateTreeTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	{
		TSharedPtr<FJsonObject> S1 = MakeShared<FJsonObject>();
		S1->SetStringField(TEXT("kind"), TEXT("state"));
		S1->SetStringField(TEXT("id"), TEXT("d1"));
		S1->SetStringField(TEXT("name"), TEXT("DeltaTest_State1"));
		Nodes.Add(STDeltaTest_ObjVal(S1));
		TSharedPtr<FJsonObject> S2 = MakeShared<FJsonObject>();
		S2->SetStringField(TEXT("kind"), TEXT("state"));
		S2->SetStringField(TEXT("id"), TEXT("d2"));
		S2->SetStringField(TEXT("name"), TEXT("DeltaTest_State2"));
		Nodes.Add(STDeltaTest_ObjVal(S2));
	}
	Args->SetArrayField(TEXT("nodes"), Nodes);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	const TSharedPtr<FJsonObject>* IdMap = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("id_map"), IdMap));
	UNTEST_EXPECT_TRUE((*IdMap)->HasField(TEXT("d1")));
	UNTEST_EXPECT_TRUE((*IdMap)->HasField(TEXT("d2")));

	const int32 AfterCount = STDeltaTest_CountSubTrees(ED);
	UNTEST_EXPECT_EQ(AfterCount, BeforeCount + 2);

	STDeltaTest_CloseSession(SessionId);
	co_return;
}

// 4. AR4 dual-shape: top-level transitions[] AND phase-4 connections[]; dedupe by id, top-level wins.
UNTEST_UNIT_OPTS(Claireon, StateTreeApplyDelta, AR4_DedupeById, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonStateTreeTool_ApplyDeltaTests_anon;
	const FString SessionId = STDeltaTest_OpenSession();
	if (SessionId.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[StateTreeApplyDelta] Test asset not present; skipping."));
		co_return;
	}
	UStateTreeEditorData* ED = STDeltaTest_GetEditorData(SessionId);
	UNTEST_ASSERT_PTR(ED);

	// First create two states, then exercise the union+dedupe in phase 4.
	FClaireonStateTreeTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	{
		TSharedPtr<FJsonObject> S1 = MakeShared<FJsonObject>();
		S1->SetStringField(TEXT("kind"), TEXT("state"));
		S1->SetStringField(TEXT("id"), TEXT("a1"));
		S1->SetStringField(TEXT("name"), TEXT("DeltaTest_AR4_A"));
		Nodes.Add(STDeltaTest_ObjVal(S1));
		TSharedPtr<FJsonObject> S2 = MakeShared<FJsonObject>();
		S2->SetStringField(TEXT("kind"), TEXT("state"));
		S2->SetStringField(TEXT("id"), TEXT("a2"));
		S2->SetStringField(TEXT("name"), TEXT("DeltaTest_AR4_B"));
		Nodes.Add(STDeltaTest_ObjVal(S2));
	}
	Args->SetArrayField(TEXT("nodes"), Nodes);

	// AR4: both top-level transitions[] AND phase-4 connections[], duplicate ids -- top-level wins.
	TArray<TSharedPtr<FJsonValue>> TopTransitions;
	{
		TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
		T->SetStringField(TEXT("id"), TEXT("tx-shared"));
		T->SetStringField(TEXT("from_state"), TEXT("a1"));
		T->SetStringField(TEXT("to_state"), TEXT("a2"));
		TopTransitions.Add(STDeltaTest_ObjVal(T));
	}
	Args->SetArrayField(TEXT("transitions"), TopTransitions);

	TArray<TSharedPtr<FJsonValue>> Phase4Connections;
	{
		TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
		T->SetStringField(TEXT("id"), TEXT("tx-shared"));
		T->SetStringField(TEXT("from_state"), TEXT("a1"));
		T->SetStringField(TEXT("to_state"), TEXT("a2"));
		T->SetStringField(TEXT("trigger"), TEXT("OnTick"));
		Phase4Connections.Add(STDeltaTest_ObjVal(T));
	}
	Args->SetArrayField(TEXT("connections"), Phase4Connections);

	const int32 BeforeTransitions = STDeltaTest_CountAllTransitions(ED);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	int32 ConnectionsMade = -1;
	Result.Data->TryGetNumberField(TEXT("connections_made"), ConnectionsMade);
	UNTEST_EXPECT_EQ(ConnectionsMade, 1); // Dedupe-by-id collapsed to one transition.

	const int32 AfterTransitions = STDeltaTest_CountAllTransitions(ED);
	UNTEST_EXPECT_EQ(AfterTransitions, BeforeTransitions + 1);

	STDeltaTest_CloseSession(SessionId);
	co_return;
}

// 5. AR4 dedupe by from->to pair when no id present.
UNTEST_UNIT_OPTS(Claireon, StateTreeApplyDelta, AR4_DedupeByFromTo, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonStateTreeTool_ApplyDeltaTests_anon;
	const FString SessionId = STDeltaTest_OpenSession();
	if (SessionId.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[StateTreeApplyDelta] Test asset not present; skipping."));
		co_return;
	}
	UStateTreeEditorData* ED = STDeltaTest_GetEditorData(SessionId);
	UNTEST_ASSERT_PTR(ED);

	FClaireonStateTreeTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	{
		TSharedPtr<FJsonObject> S1 = MakeShared<FJsonObject>();
		S1->SetStringField(TEXT("kind"), TEXT("state"));
		S1->SetStringField(TEXT("id"), TEXT("p1"));
		S1->SetStringField(TEXT("name"), TEXT("DeltaTest_AR4FromTo_A"));
		Nodes.Add(STDeltaTest_ObjVal(S1));
		TSharedPtr<FJsonObject> S2 = MakeShared<FJsonObject>();
		S2->SetStringField(TEXT("kind"), TEXT("state"));
		S2->SetStringField(TEXT("id"), TEXT("p2"));
		S2->SetStringField(TEXT("name"), TEXT("DeltaTest_AR4FromTo_B"));
		Nodes.Add(STDeltaTest_ObjVal(S2));
	}
	Args->SetArrayField(TEXT("nodes"), Nodes);

	TArray<TSharedPtr<FJsonValue>> TopTransitions;
	{
		TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
		T->SetStringField(TEXT("from_state"), TEXT("p1"));
		T->SetStringField(TEXT("to_state"), TEXT("p2"));
		TopTransitions.Add(STDeltaTest_ObjVal(T));
	}
	Args->SetArrayField(TEXT("transitions"), TopTransitions);

	TArray<TSharedPtr<FJsonValue>> Phase4Connections;
	{
		TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
		T->SetStringField(TEXT("from_state"), TEXT("p1"));
		T->SetStringField(TEXT("to_state"), TEXT("p2"));
		Phase4Connections.Add(STDeltaTest_ObjVal(T));
	}
	Args->SetArrayField(TEXT("connections"), Phase4Connections);

	const int32 BeforeTransitions = STDeltaTest_CountAllTransitions(ED);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	int32 ConnectionsMade = -1;
	Result.Data->TryGetNumberField(TEXT("connections_made"), ConnectionsMade);
	UNTEST_EXPECT_EQ(ConnectionsMade, 1);
	const int32 AfterTransitions = STDeltaTest_CountAllTransitions(ED);
	UNTEST_EXPECT_EQ(AfterTransitions, BeforeTransitions + 1);

	STDeltaTest_CloseSession(SessionId);
	co_return;
}

#endif // WITH_UNTESTED
