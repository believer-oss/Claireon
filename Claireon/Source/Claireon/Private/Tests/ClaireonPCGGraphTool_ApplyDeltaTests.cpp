// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Functional tests for pcg_apply_delta (#0000).

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonPCGTool_ApplyDelta.h"
#include "Tools/ClaireonPCGGraphTool_Open.h"
#include "Tools/ClaireonPCGGraphTool_Close.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/ClaireonPCGGraphEditToolBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "PCGGraph.h"
#include "PCGNode.h"

namespace ClaireonPCGTool_ApplyDeltaTests_anon
{
	static const TCHAR* PCGDeltaTestAssetPath = TEXT("/Game/__MCPTests/PCG_ApplySpecTest");

	static TSharedPtr<FJsonValue> PCGDeltaTest_ObjVal(const TSharedPtr<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }

	static FString PCGDeltaTest_OpenSession()
	{
		ClaireonPCGGraphTool_Open OpenTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), PCGDeltaTestAssetPath);
		auto Result = OpenTool.Execute(Args);
		if (Result.bIsError || !Result.Data.IsValid()) { return FString(); }
		FString SessionId;
		Result.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	static void PCGDeltaTest_CloseSession(const FString& SessionId)
	{
		if (SessionId.IsEmpty()) { return; }
		ClaireonPCGGraphTool_Close CloseTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		CloseTool.Execute(Args);
	}
}

// 1. Validation error: missing both session_id and asset_path.
UNTEST_UNIT_OPTS(Claireon, PCGApplyDelta, MissingSessionAndAssetPath, UNTEST_TIMEOUTMS(10000))
{
	FClaireonPCGTool_ApplyDelta Tool;
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
UNTEST_UNIT_OPTS(Claireon, PCGApplyDelta, FailOnMissingAsset, UNTEST_TIMEOUTMS(15000))
{
	FClaireonPCGTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/__definitely_does_not_exist__PCG_zz"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

// 3. Empty input via existing session is a no-op (asset-dependent; skipped if not present).
UNTEST_UNIT_OPTS(Claireon, PCGApplyDelta, EmptyIsNoOp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonPCGTool_ApplyDeltaTests_anon;
	const FString SessionId = PCGDeltaTest_OpenSession();
	if (SessionId.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[PCGApplyDelta] Test asset not present; skipping."));
		co_return;
	}

	FClaireonPCGTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	auto Result = Tool.Execute(Args);

	UNTEST_EXPECT_FALSE(Result.bIsError);

	PCGDeltaTest_CloseSession(SessionId);
	co_return;
}

// 4. Phase-3 failure with invalid type -> transaction cancels, asset unchanged.
UNTEST_UNIT_OPTS(Claireon, PCGApplyDelta, Phase3FailureRollsBack, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonPCGTool_ApplyDeltaTests_anon;
	const FString SessionId = PCGDeltaTest_OpenSession();
	if (SessionId.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[PCGApplyDelta] Test asset not present; skipping."));
		co_return;
	}
	FPCGGraphEditToolData* SessionData = ClaireonPCGGraphEditToolBase::ToolData.Find(SessionId);
	UNTEST_ASSERT_PTR(SessionData);
	UPCGGraph* GraphPtr = SessionData->PCGGraph.Get();
	UNTEST_ASSERT_PTR(GraphPtr);
	const int32 BeforeCount = GraphPtr->GetNodes().Num();

	FClaireonPCGTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	{
		TSharedPtr<FJsonObject> N1 = MakeShared<FJsonObject>();
		N1->SetStringField(TEXT("id"), TEXT("n1"));
		N1->SetStringField(TEXT("type"), TEXT("PCGSurfaceSamplerSettings"));
		Nodes.Add(PCGDeltaTest_ObjVal(N1));
		TSharedPtr<FJsonObject> NBad = MakeShared<FJsonObject>();
		NBad->SetStringField(TEXT("id"), TEXT("nbad"));
		NBad->SetStringField(TEXT("type"), TEXT("PCGDoesNotExistSettings_xyz"));
		Nodes.Add(PCGDeltaTest_ObjVal(NBad));
	}
	Args->SetArrayField(TEXT("nodes"), Nodes);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("3"));

	// After rollback, the asset should have the same node count as before.
	const int32 AfterCount = GraphPtr->GetNodes().Num();
	UNTEST_EXPECT_EQ(AfterCount, BeforeCount);

	PCGDeltaTest_CloseSession(SessionId);
	co_return;
}

#endif // WITH_UNTESTED
