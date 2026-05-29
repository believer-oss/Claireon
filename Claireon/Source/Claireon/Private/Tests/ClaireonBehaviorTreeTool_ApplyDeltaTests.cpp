// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Functional tests for behaviortree_apply_delta (#0000).
// Mirrors ClaireonBehaviorTreeTool_ApplySpec test patterns.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonBehaviorTreeTool_ApplyDelta.h"
#include "Tools/ClaireonBehaviorTreeTool_Open.h"
#include "Tools/ClaireonBehaviorTreeTool_Close.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "Tools/ClaireonBehaviorTreeEditToolBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "BehaviorTreeGraphNode.h"

namespace ClaireonBehaviorTreeTool_ApplyDeltaTests_anon
{
	static const TCHAR* BTDeltaTestAssetPath = TEXT("/Game/BP/AI/BT/BT_CombatAttacking_Default");

	static TSharedPtr<FJsonValue> BTDeltaTest_StrVal(const FString& V) { return MakeShared<FJsonValueString>(V); }
	static TSharedPtr<FJsonValue> BTDeltaTest_ObjVal(const TSharedPtr<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }

	static FString BTDeltaTest_OpenSession()
	{
		ClaireonBehaviorTreeTool_Open OpenTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), BTDeltaTestAssetPath);
		auto Result = OpenTool.Execute(Args);
		if (Result.bIsError || !Result.Data.IsValid()) { return FString(); }
		FString SessionId;
		Result.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	static void BTDeltaTest_CloseSession(const FString& SessionId)
	{
		if (SessionId.IsEmpty()) { return; }
		ClaireonBehaviorTreeTool_Close CloseTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		CloseTool.Execute(Args);
	}

	static int32 BTDeltaTest_CountNodes(const FString& SessionId)
	{
		FBehaviorTreeEditToolData* Data = ClaireonBehaviorTreeEditToolBase::ToolData.Find(SessionId);
		if (!Data) { return -1; }
		UBehaviorTreeGraph* Graph = Data->BTGraph.Get();
		if (!Graph) { return -1; }
		return Graph->Nodes.Num();
	}
}

// 1. Validation error: missing both session_id and asset_path.
UNTEST_UNIT_OPTS(Claireon, BehaviorTreeApplyDelta, MissingSessionAndAssetPath, UNTEST_TIMEOUTMS(10000))
{
	FClaireonBehaviorTreeTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("validate"));
	co_return;
}

// 2. M5 fail-on-missing: bogus asset_path returns an error before any phase runs.
UNTEST_UNIT_OPTS(Claireon, BehaviorTreeApplyDelta, FailOnMissingAsset, UNTEST_TIMEOUTMS(15000))
{
	FClaireonBehaviorTreeTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/__definitely_does_not_exist__BT_zz"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

// 3. Empty input via existing session is a no-op success (AR9).
UNTEST_UNIT_OPTS(Claireon, BehaviorTreeApplyDelta, EmptyIsNoOp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonBehaviorTreeTool_ApplyDeltaTests_anon;
	const FString SessionId = BTDeltaTest_OpenSession();
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	FClaireonBehaviorTreeTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_FALSE(Result.bIsError);
	if (Result.Data.IsValid())
	{
		int32 Created = -1, Removed = -1, Connected = -1;
		Result.Data->TryGetNumberField(TEXT("created_count"), Created);
		Result.Data->TryGetNumberField(TEXT("removed_count"), Removed);
		Result.Data->TryGetNumberField(TEXT("connections_made"), Connected);
		UNTEST_EXPECT_EQ(Created, 0);
		UNTEST_EXPECT_EQ(Removed, 0);
		UNTEST_EXPECT_EQ(Connected, 0);
	}

	BTDeltaTest_CloseSession(SessionId);
	co_return;
}

// 4. Happy path: create two task nodes via phase 3; both nodes appear in graph; id_map populated.
UNTEST_UNIT_OPTS(Claireon, BehaviorTreeApplyDelta, CreateNodesPhase3, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonBehaviorTreeTool_ApplyDeltaTests_anon;
	const FString SessionId = BTDeltaTest_OpenSession();
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const int32 BeforeCount = BTDeltaTest_CountNodes(SessionId);
	UNTEST_ASSERT_TRUE(BeforeCount >= 1);

	FClaireonBehaviorTreeTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	{
		TSharedPtr<FJsonObject> N1 = MakeShared<FJsonObject>();
		N1->SetStringField(TEXT("id"), TEXT("t1"));
		N1->SetStringField(TEXT("class"), TEXT("BTTask_Wait"));
		Nodes.Add(BTDeltaTest_ObjVal(N1));
		TSharedPtr<FJsonObject> N2 = MakeShared<FJsonObject>();
		N2->SetStringField(TEXT("id"), TEXT("t2"));
		N2->SetStringField(TEXT("class"), TEXT("BTTask_Wait"));
		Nodes.Add(BTDeltaTest_ObjVal(N2));
	}
	Args->SetArrayField(TEXT("nodes"), Nodes);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	const TSharedPtr<FJsonObject>* IdMap = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("id_map"), IdMap));
	UNTEST_EXPECT_TRUE((*IdMap)->HasField(TEXT("t1")));
	UNTEST_EXPECT_TRUE((*IdMap)->HasField(TEXT("t2")));

	const int32 AfterCount = BTDeltaTest_CountNodes(SessionId);
	UNTEST_EXPECT_TRUE(AfterCount == BeforeCount + 2);

	BTDeltaTest_CloseSession(SessionId);
	co_return;
}

// 5. Phase-3 failure with invalid class -> transaction cancels, asset state unchanged after rollback.
UNTEST_UNIT_OPTS(Claireon, BehaviorTreeApplyDelta, Phase3FailureRollsBack, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonBehaviorTreeTool_ApplyDeltaTests_anon;
	const FString SessionId = BTDeltaTest_OpenSession();
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const int32 BeforeCount = BTDeltaTest_CountNodes(SessionId);

	FClaireonBehaviorTreeTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	{
		TSharedPtr<FJsonObject> N1 = MakeShared<FJsonObject>();
		N1->SetStringField(TEXT("id"), TEXT("g1"));
		N1->SetStringField(TEXT("class"), TEXT("BTTask_Wait"));
		Nodes.Add(BTDeltaTest_ObjVal(N1));
		TSharedPtr<FJsonObject> NBad = MakeShared<FJsonObject>();
		NBad->SetStringField(TEXT("id"), TEXT("g2"));
		NBad->SetStringField(TEXT("class"), TEXT("ThisClassDoesNotExist_xyz"));
		Nodes.Add(BTDeltaTest_ObjVal(NBad));
	}
	Args->SetArrayField(TEXT("nodes"), Nodes);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("3"));

	// Asset state unchanged after rollback.
	const int32 AfterCount = BTDeltaTest_CountNodes(SessionId);
	UNTEST_EXPECT_EQ(AfterCount, BeforeCount);

	BTDeltaTest_CloseSession(SessionId);
	co_return;
}

#endif // WITH_UNTESTED
