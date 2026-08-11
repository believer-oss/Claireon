// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Stage 040 (N3a): a read-only Blueprint inspector must not register a blocking
// session.
//
// The reported failure: bp_get_component_details, which describes itself as
// read-only, called BeginSessionOp and so registered a session with
// FClaireonSessionManager. The bridge then refused every bypass-mode tool --
// console_execute among them -- until that session timed out or was force-released.
// An inspection is not a reason to lock an asset.
//
// The assertion that actually pins this is the session-count one. Everything else
// here is about the response staying honest once no session exists to describe.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonBlueprintGraphTool_AddComponent.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_GetComponentDetails.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonSessionManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonBPReadOnlySessionTestsNS
{
	// File-local named namespace (not anonymous): anonymous namespaces from separate
	// .cpp merge and collide under unity batching on v2.

	// True only when the fixture actually has a .uasset on disk.
	//
	// This suite's fixture is built by bp_create + bp_add_component; neither saves
	// (ClaireonBlueprintHelpers::CreateBlueprint has no save call and
	// ClaireonBlueprintGraphTool_AddComponent only saves via bp_save/bp_close_all,
	// which these tests never call), so the fixture normally lives in an in-memory
	// package only. Deleting an in-memory fixture buys nothing, and every
	// ObjectTools::ForceDeleteObjects call runs a whole-object-graph referencer
	// scan, which is the trigger for the nondeterministic Niagara-serialization
	// crash documented in
	// Docs/llm/todo/claireon-untest-harness-reliability.md item 1.
	//
	// The check is kept rather than dropping the delete outright because
	// /Game/__MCPTests is deliberately NOT gitignored: a stale .uasset left by an
	// older build or a crashed run must still be cleaned so `git status
	// --porcelain -- Content/` stays empty.
	static bool ROS_HasFileOnDisk(const FString& AssetOrPackagePath)
	{
		const FString PackageName = FPackageName::ObjectPathToPackageName(AssetOrPackagePath);
		FString FileName;
		if (!FPackageName::TryConvertLongPackageNameToFilename(
				PackageName, FileName, FPackageName::GetAssetPackageExtension()))
		{
			return false;
		}
		return FPaths::FileExists(FileName);
	}

	static void ROS_CleanupAsset(const FString& AssetPath)
	{
		// The session release is unconditional: it is what makes the read-only call
		// start from a clean slate, and it has nothing to do with disk state.
		FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);

		// In-memory fixture: nothing on disk, nothing to clean, no referencer scan.
		if (!ROS_HasFileOnDisk(AssetPath))
		{
			return;
		}

		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> AssetsToDelete;
			AssetsToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
		}
	}

	/**
	 * Create a scratch Actor Blueprint carrying one component, then release every
	 * session the setup opened so the read-only call starts from a clean slate.
	 * Returns false if setup failed.
	 */
	static bool ROS_MakeAssetWithComponent(const FString& AssetPath, const FString& ComponentName)
	{
		{
			ClaireonBlueprintGraphTool_Create CreateTool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("asset_path"), AssetPath);
			Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
			if (CreateTool.Execute(Args).bIsError) { return false; }
		}
		{
			ClaireonBlueprintGraphTool_AddComponent AddTool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("asset_path"), AssetPath);
			Args->SetStringField(TEXT("component_name"), ComponentName);
			Args->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.StaticMeshComponent"));
			if (AddTool.Execute(Args).bIsError) { return false; }
		}
		FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);
		return true;
	}

	static IClaireonTool::FToolResult ROS_GetDetailsByAssetPath(
		const FString& AssetPath, const FString& ComponentName)
	{
		ClaireonBlueprintGraphTool_GetComponentDetails Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("component_name"), ComponentName);
		return Tool.Execute(Args);
	}
}

UNTEST_UNIT_OPTS(Claireon, BPReadOnlySession, GetComponentDetailsOpensNoSession, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPReadOnlySessionTestsNS;

	const FString AssetPath = TEXT("/Game/__MCPTests/BP_ReadOnlyNoSession");
	const FString ComponentName = TEXT("ROSMesh");

	ROS_CleanupAsset(AssetPath);
	const bool bSetup = ROS_MakeAssetWithComponent(AssetPath, ComponentName);

	const int32 SessionsBefore = FClaireonSessionManager::Get().ListSessions().Num();
	IClaireonTool::FToolResult Result = ROS_GetDetailsByAssetPath(AssetPath, ComponentName);
	const int32 SessionsAfter = FClaireonSessionManager::Get().ListSessions().Num();

	ROS_CleanupAsset(AssetPath);

	UNTEST_ASSERT_TRUE(bSetup);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	// THE assertion. A read-only inspection that leaves a session behind blocks every
	// bypass-mode tool until it expires.
	UNTEST_EXPECT_EQ(SessionsAfter, SessionsBefore);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPReadOnlySession, ReadOnlyResponseDoesNotClaimASession, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPReadOnlySessionTestsNS;

	const FString AssetPath = TEXT("/Game/__MCPTests/BP_ReadOnlyResponse");
	const FString ComponentName = TEXT("ROSMesh");

	ROS_CleanupAsset(AssetPath);
	const bool bSetup = ROS_MakeAssetWithComponent(AssetPath, ComponentName);

	IClaireonTool::FToolResult Result = ROS_GetDetailsByAssetPath(AssetPath, ComponentName);

	ROS_CleanupAsset(AssetPath);

	UNTEST_ASSERT_TRUE(bSetup);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	// Stated positively: a caller who read an empty session_id and passed it back would
	// get "Invalid or expired session_id" and have no idea why.
	bool bReadOnly = false;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetBoolField(TEXT("read_only"), bReadOnly));
	UNTEST_EXPECT_TRUE(bReadOnly);

	FString SessionId;
	Result.Data->TryGetStringField(TEXT("session_id"), SessionId);
	UNTEST_EXPECT_TRUE(SessionId.IsEmpty());

	// Skipping the session was not allowed to cost the caller the actual payload.
	const TSharedPtr<FJsonObject>* Component = nullptr;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetObjectField(TEXT("component"), Component));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPReadOnlySession, ExplicitSessionIdStillReadsInsideThatSession, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPReadOnlySessionTestsNS;

	// The read-only path must not punish a caller who already owns a session: reusing it
	// is correct, and the response has to keep reporting the session state they track.
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_ReadOnlyWithSession");
	const FString ComponentName = TEXT("ROSMesh");

	ROS_CleanupAsset(AssetPath);

	bool bSetup = false;
	FString OpenedSessionId;
	{
		ClaireonBlueprintGraphTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		bSetup = !CreateTool.Execute(Args).bIsError;
	}
	if (bSetup)
	{
		ClaireonBlueprintGraphTool_AddComponent AddTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("component_name"), ComponentName);
		Args->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.StaticMeshComponent"));
		IClaireonTool::FToolResult AddResult = AddTool.Execute(Args);
		bSetup = !AddResult.bIsError;
		if (AddResult.Data.IsValid())
		{
			AddResult.Data->TryGetStringField(TEXT("session_id"), OpenedSessionId);
		}
	}

	IClaireonTool::FToolResult Result;
	if (!OpenedSessionId.IsEmpty())
	{
		ClaireonBlueprintGraphTool_GetComponentDetails Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), OpenedSessionId);
		Args->SetStringField(TEXT("component_name"), ComponentName);
		Result = Tool.Execute(Args);
	}

	ROS_CleanupAsset(AssetPath);

	UNTEST_ASSERT_TRUE(bSetup);
	UNTEST_ASSERT_FALSE(OpenedSessionId.IsEmpty());
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	FString ReportedSessionId;
	Result.Data->TryGetStringField(TEXT("session_id"), ReportedSessionId);
	UNTEST_EXPECT_EQ(ReportedSessionId, OpenedSessionId);

	// read_only marks "no session was opened"; it must not appear when one is in use.
	bool bReadOnly = false;
	UNTEST_EXPECT_FALSE(Result.Data->TryGetBoolField(TEXT("read_only"), bReadOnly));

	co_return;
}

#endif // WITH_UNTESTED
