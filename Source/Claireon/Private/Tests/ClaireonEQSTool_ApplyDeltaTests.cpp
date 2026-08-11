// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Functional tests for eqs_apply_delta.
// Verifies AR5/AR9 phase-1/4 rejection at the base-driver layer plus M5 fail-on-missing.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonEQSTool_ApplyDelta.h"
#include "Tools/ClaireonEQSTool_Open.h"
#include "Tools/ClaireonEQSTool_Close.h"
#include "Tools/ClaireonEQSEditToolBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "Misc/ScopeExit.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonEQSTool_ApplyDeltaTests_anon
{
	// The source is READ-ONLY. eqs_apply_delta writes through to the asset, so
	// running it against shipping content dirties a tracked .uasset on every run
	// and leaves the asset mutated for whoever inspects it later -- that is what
	// made Claireon.EQS.InspectStrafe pass alone and fail in a full run.
	static const TCHAR* EQSDeltaSourceAssetPath = TEXT("/Game/BP/AI/EQS/EQS_CombatWaiting_Strafe");
	static const TCHAR* EQSDeltaTestAssetPath   = TEXT("/Game/__MCPTests/EQS_ApplyDeltaTest");

	// Delete any copy left behind by a previous run before duplicating:
	// /Game/__MCPTests is NOT gitignored and persists, so a run that dies mid-test
	// would otherwise hand a mutated fixture to the next one.
	static bool EQSDeltaTest_EnsureFreshDuplicate()
	{
		if (UEditorAssetLibrary::DoesAssetExist(EQSDeltaTestAssetPath))
		{
			ClaireonTestAssetDeletion::DeleteAssetForTest(EQSDeltaTestAssetPath);
		}
		return UEditorAssetLibrary::DuplicateAsset(EQSDeltaSourceAssetPath, EQSDeltaTestAssetPath) != nullptr;
	}

	static void EQSDeltaTest_DeleteDuplicate()
	{
		if (UEditorAssetLibrary::DoesAssetExist(EQSDeltaTestAssetPath))
		{
			ClaireonTestAssetDeletion::DeleteAssetForTest(EQSDeltaTestAssetPath);
		}
	}

	static TSharedPtr<FJsonValue> EQSDeltaTest_ObjVal(const TSharedPtr<FJsonObject>& O) { return MakeShared<FJsonValueObject>(O); }

	static FString EQSDeltaTest_OpenSession()
	{
		ClaireonEQSTool_Open OpenTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), EQSDeltaTestAssetPath);
		auto Result = OpenTool.Execute(Args);
		if (Result.bIsError || !Result.Data.IsValid()) { return FString(); }
		FString SessionId;
		Result.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	static void EQSDeltaTest_CloseSession(const FString& SessionId)
	{
		if (SessionId.IsEmpty()) { return; }
		ClaireonEQSTool_Close CloseTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		CloseTool.Execute(Args);
	}
}

// 1. Validation error: missing both session_id and asset_path.
UNTEST_UNIT_OPTS(Claireon, EQSApplyDelta, MissingSessionAndAssetPath, UNTEST_TIMEOUTMS(10000))
{
	FClaireonEQSTool_ApplyDelta Tool;
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
UNTEST_UNIT_OPTS(Claireon, EQSApplyDelta, FailOnMissingAsset, UNTEST_TIMEOUTMS(15000))
{
	FClaireonEQSTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/__definitely_does_not_exist__EQS_zz"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

// 3. AR5: SupportsPhase1Disconnect()==false rejects non-empty disconnect[].
UNTEST_UNIT_OPTS(Claireon, EQSApplyDelta, RejectsPhase1NonEmpty, UNTEST_TIMEOUTMS(10000))
{
	FClaireonEQSTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	// session_id placeholder; the validation error fires before session lookup.
	Args->SetStringField(TEXT("session_id"), TEXT("does-not-exist-session-id"));

	TArray<TSharedPtr<FJsonValue>> Disc;
	{
		TSharedPtr<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetStringField(TEXT("parent_id"), TEXT("p"));
		D->SetStringField(TEXT("child_id"), TEXT("c"));
		Disc.Add(ClaireonEQSTool_ApplyDeltaTests_anon::EQSDeltaTest_ObjVal(D));
	}
	Args->SetArrayField(TEXT("disconnect"), Disc);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("validate"));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("disconnect")));
	co_return;
}

// 4. AR5: SupportsPhase4Connect()==false rejects non-empty connections[].
UNTEST_UNIT_OPTS(Claireon, EQSApplyDelta, RejectsPhase4NonEmpty, UNTEST_TIMEOUTMS(10000))
{
	FClaireonEQSTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), TEXT("does-not-exist-session-id"));

	TArray<TSharedPtr<FJsonValue>> Conns;
	{
		TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("parent_id"), TEXT("p"));
		C->SetStringField(TEXT("child_id"), TEXT("c"));
		Conns.Add(ClaireonEQSTool_ApplyDeltaTests_anon::EQSDeltaTest_ObjVal(C));
	}
	Args->SetArrayField(TEXT("connections"), Conns);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());
	FString FailedPhase;
	Result.Data->TryGetStringField(TEXT("failed_phase"), FailedPhase);
	UNTEST_EXPECT_STREQ(FailedPhase, TEXT("validate"));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("connect")));
	co_return;
}

// 5. AR9: empty disconnect[] and connections[] arrays are accepted (no-op success).
UNTEST_UNIT_OPTS(Claireon, EQSApplyDelta, AR9_EmptyArraysAccepted, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonEQSTool_ApplyDeltaTests_anon;

	// Operate on a throwaway duplicate: this test applies a delta, which writes
	// through to the asset.
	EQSDeltaTest_EnsureFreshDuplicate();
	ON_SCOPE_EXIT { EQSDeltaTest_DeleteDuplicate(); };

	const FString SessionId = EQSDeltaTest_OpenSession();
	if (SessionId.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[EQSApplyDelta] Test asset not present; skipping."));
		co_return;
	}

	FClaireonEQSTool_ApplyDelta Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	TArray<TSharedPtr<FJsonValue>> Empty;
	Args->SetArrayField(TEXT("disconnect"), Empty);
	Args->SetArrayField(TEXT("connections"), Empty);

	auto Result = Tool.Execute(Args);
	UNTEST_EXPECT_FALSE(Result.bIsError);

	EQSDeltaTest_CloseSession(SessionId);
	co_return;
}

#endif // WITH_UNTESTED
