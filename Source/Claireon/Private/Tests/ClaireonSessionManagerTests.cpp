// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonSessionManager.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace ClaireonSessionManagerTest
{
	static void CleanupAllSessions()
	{
		FClaireonSessionManager::Get().ForceReleaseAll();
	}
} // namespace ClaireonSessionManagerTest

// ============================================================================
// SessionManager Tests
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, SessionManager, OpenCloseLifecycle, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_TestAsset"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(OpenResult.Result == EOpenSessionResult::Success);
	UNTEST_EXPECT_TRUE(!OpenResult.SessionId.IsEmpty());

	FMCPSession* Found = FClaireonSessionManager::Get().FindSession(OpenResult.SessionId);
	UNTEST_ASSERT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found->ToolName == TEXT("test_edit"));
	UNTEST_EXPECT_TRUE(Found->AssetPath == TEXT("/Game/Test/BP_TestAsset"));

	bool bClosed = FClaireonSessionManager::Get().CloseSession(OpenResult.SessionId);
	UNTEST_EXPECT_TRUE(bClosed);

	FMCPSession* AfterClose = FClaireonSessionManager::Get().FindSession(OpenResult.SessionId);
	UNTEST_EXPECT_TRUE(AfterClose == nullptr);

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, SameToolReopen, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Reopen"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Reopen"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);
	UNTEST_EXPECT_TRUE(Second.SessionId == First.SessionId);

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, DifferentToolBlock, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Block"), TEXT("toolA_edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Block"), TEXT("toolB_edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::BlockedByOtherTool);
	UNTEST_EXPECT_TRUE(Second.SessionId.IsEmpty());
	UNTEST_EXPECT_TRUE(Second.BlockingSession.IsSet());
	UNTEST_EXPECT_TRUE(Second.BlockingSession->ToolName == TEXT("toolA_edit"));

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, PathCanonicalization, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Foo.uasset"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Foo"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);
	UNTEST_EXPECT_TRUE(Second.SessionId == First.SessionId);

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, PathCanonObjectNameSuffix, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Foo.BP_Foo"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Foo"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);
	UNTEST_EXPECT_TRUE(Second.SessionId == First.SessionId);

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, InvalidPathRejection, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	FMCPOpenSessionResult Result = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Script/Engine.Actor"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(Result.Result == EOpenSessionResult::InvalidAssetPath);
	UNTEST_EXPECT_TRUE(Result.SessionId.IsEmpty());

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, IsAssetLocked, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_LockTest"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(OpenResult.Result == EOpenSessionResult::Success);

	bool bLocked = FClaireonSessionManager::Get().IsAssetLocked(TEXT("/Game/Test/BP_LockTest"));
	UNTEST_EXPECT_TRUE(bLocked);

	FClaireonSessionManager::Get().CloseSession(OpenResult.SessionId);

	bool bLockedAfter = FClaireonSessionManager::Get().IsAssetLocked(TEXT("/Game/Test/BP_LockTest"));
	UNTEST_EXPECT_TRUE(!bLockedAfter);

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, ListSessions, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	FClaireonSessionManager::Get().OpenSession(TEXT("/Game/Test/BP_A"), TEXT("bp"));
	FClaireonSessionManager::Get().OpenSession(TEXT("/Game/Test/BP_B"), TEXT("niagara_edit"));

	TArray<FMCPSession> All = FClaireonSessionManager::Get().ListSessions();
	UNTEST_EXPECT_TRUE(All.Num() == 2);

	TArray<FMCPSession> BPOnly = FClaireonSessionManager::Get().ListSessions(TEXT("bp"));
	UNTEST_EXPECT_TRUE(BPOnly.Num() == 1);

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, ReleaseByAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Release"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(OpenResult.Result == EOpenSessionResult::Success);

	int32 Released = FClaireonSessionManager::Get().ReleaseByAssetPath(TEXT("/Game/Test/BP_Release"));
	UNTEST_EXPECT_TRUE(Released == 1);

	FMCPSession* AfterRelease = FClaireonSessionManager::Get().FindSession(OpenResult.SessionId);
	UNTEST_EXPECT_TRUE(AfterRelease == nullptr);

	int32 ReleasedAgain = FClaireonSessionManager::Get().ReleaseByAssetPath(TEXT("/Game/Test/BP_Release"));
	UNTEST_EXPECT_TRUE(ReleasedAgain == 0);

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, ForceReleaseAll, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	FClaireonSessionManager::Get().OpenSession(TEXT("/Game/Test/BP_1"), TEXT("test_edit"));
	FClaireonSessionManager::Get().OpenSession(TEXT("/Game/Test/BP_2"), TEXT("test_edit"));
	FClaireonSessionManager::Get().OpenSession(TEXT("/Game/Test/BP_3"), TEXT("test_edit"));

	int32 Count = FClaireonSessionManager::Get().ForceReleaseAll();
	UNTEST_EXPECT_TRUE(Count == 3);

	TArray<FMCPSession> Remaining = FClaireonSessionManager::Get().ListSessions();
	UNTEST_EXPECT_TRUE(Remaining.Num() == 0);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, Expiry, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	// Open with short timeout
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Expiry"), TEXT("test_edit"), 30.0);
	UNTEST_ASSERT_TRUE(OpenResult.Result == EOpenSessionResult::Success);

	// Get the session pointer and manually set LastAccessTime far in the past
	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(OpenResult.SessionId);
	UNTEST_ASSERT_TRUE(Session != nullptr);
	Session->LastAccessTime = FDateTime::UtcNow() - FTimespan::FromMinutes(60.0);

	// Now FindSession should detect the session as expired and remove it
	FMCPSession* Found = FClaireonSessionManager::Get().FindSession(OpenResult.SessionId);
	UNTEST_EXPECT_TRUE(Found == nullptr);

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, CloseSessionNoOp, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	bool bResult = FClaireonSessionManager::Get().CloseSession(TEXT("nonexistent-session-id"));
	UNTEST_EXPECT_TRUE(!bResult);

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, DelegateFiresOnClose, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	bool bDelegateFired = false;
	FString CapturedSessionId;
	FString CapturedAssetPath;
	FString CapturedToolName;

	FDelegateHandle Handle = FClaireonSessionManager::Get().OnSessionClosed().AddLambda(
		[&](const FMCPSessionClosedInfo& Info)
	{
		bDelegateFired = true;
		CapturedSessionId = Info.SessionId;
		CapturedAssetPath = Info.AssetPath;
		CapturedToolName = Info.ToolName;
	});

	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Delegate"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(OpenResult.Result == EOpenSessionResult::Success);

	FClaireonSessionManager::Get().CloseSession(OpenResult.SessionId);

	UNTEST_EXPECT_TRUE(bDelegateFired);
	UNTEST_EXPECT_TRUE(CapturedSessionId == OpenResult.SessionId);
	UNTEST_EXPECT_TRUE(CapturedAssetPath == TEXT("/Game/Test/BP_Delegate"));
	UNTEST_EXPECT_TRUE(CapturedToolName == TEXT("test_edit"));

	FClaireonSessionManager::Get().OnSessionClosed().Remove(Handle);
	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, DelegateDoesNotFireOnNoOp, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	bool bDelegateFired = false;

	FDelegateHandle Handle = FClaireonSessionManager::Get().OnSessionClosed().AddLambda(
		[&](const FMCPSessionClosedInfo& Info)
	{
		bDelegateFired = true;
	});

	FClaireonSessionManager::Get().CloseSession(TEXT("nonexistent-session"));
	UNTEST_EXPECT_TRUE(!bDelegateFired);

	FClaireonSessionManager::Get().OnSessionClosed().Remove(Handle);
	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, TimeoutMaxOnReopen, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Timeout"), TEXT("test_edit"), 10.0);
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Timeout"), TEXT("test_edit"), 60.0);
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);

	FMCPSession* Found = FClaireonSessionManager::Get().FindSession(First.SessionId);
	UNTEST_ASSERT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(FMath::IsNearlyEqual(Found->TimeoutMinutes, 60.0));

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

// ---------------------------------------------------------------------------
// Path canonicalization: double slashes, backslashes, trailing slash
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, SessionManager, PathCanonDoubleSlashes, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	// Double slashes should be collapsed to single slashes
	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game//Test//BP_DoubleSlash"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	// Normal path should reuse the same session
	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_DoubleSlash"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);
	UNTEST_EXPECT_TRUE(Second.SessionId == First.SessionId);

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, PathCanonBackslashes, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	// Backslashes should be normalized to forward slashes
	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game\\Test\\BP_Backslash"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Backslash"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);
	UNTEST_EXPECT_TRUE(Second.SessionId == First.SessionId);

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, PathCanonTrailingSlash, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionManagerTest::CleanupAllSessions();

	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Trailing/"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Trailing"), TEXT("test_edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);
	UNTEST_EXPECT_TRUE(Second.SessionId == First.SessionId);

	ClaireonSessionManagerTest::CleanupAllSessions();
	co_return;
}

// ---------------------------------------------------------------------------
// C5 hardening: an unsaved-world /Temp/ path canonicalizes ONLY when the caller
// explicitly opts in via bAllowUnsavedWorldPackage, and a /Temp/ path is still
// rejected by default -- this is not a blanket relaxation of the /Game/ rule.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, SessionManager, PathCanonTempWorldAllowedWhenFlagged, UNTEST_TIMEOUTMS(5000))
{
	// Bare package form, as used by ClaireonMaterialTool_ApplyToActor.cpp
	// (World->GetOutermost()->GetName()).
	const FString CanonBarePackage = FClaireonSessionManager::CanonicalizePath(
		TEXT("/Temp/Untitled_1"), /*bAllowUnsavedWorldPackage=*/true);
	UNTEST_EXPECT_TRUE(CanonBarePackage == TEXT("/Temp/Untitled_1"));

	// Object-path form, as used by ClaireonFoliageTool_Open.cpp
	// (World->PersistentLevel->GetPathName()). ULevel's outer is the UWorld (see
	// UWorld::InitializeNewWorld: "NewObject<ULevel>(this, TEXT(\"PersistentLevel\"))"),
	// so the real shape repeats the world's object name and crosses into it with a
	// colon, e.g. "/Temp/Untitled_1.Untitled_1:PersistentLevel" -- exercised here rather
	// than a simplified dot-only path so this test matches what the tool actually
	// passes. Must canonicalize to the SAME package-prefix key as the bare form above,
	// so the two world-locking tools contend for the same lock on the same unsaved world.
	const FString CanonLevelObject = FClaireonSessionManager::CanonicalizePath(
		TEXT("/Temp/Untitled_1.Untitled_1:PersistentLevel"), /*bAllowUnsavedWorldPackage=*/true);
	UNTEST_EXPECT_TRUE(CanonLevelObject == TEXT("/Temp/Untitled_1"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, PathCanonTempRejectedWithoutFlag, UNTEST_TIMEOUTMS(5000))
{
	// Same /Temp/ path as above, default flag (false): must still be rejected. This is
	// the behavior for every one of the ~40 other CanonicalizePath callers that have NOT
	// identified their target as the world package -- the regression this guards is a
	// blanket /Temp/ relaxation creeping in where only explicit world-package callers
	// should get it.
	const FString Canon = FClaireonSessionManager::CanonicalizePath(TEXT("/Temp/Untitled_1"));
	UNTEST_EXPECT_TRUE(Canon.IsEmpty());

	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Temp/Untitled_1"), TEXT("test_edit"));
	UNTEST_EXPECT_TRUE(OpenResult.Result == EOpenSessionResult::InvalidAssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, PathCanonGameStillWorksWithFlagSet, UNTEST_TIMEOUTMS(5000))
{
	// The flag must not change behavior for an ordinary /Game/ path -- it only ADDS a
	// /Temp/ allowance, it does not touch the existing /Game/ acceptance path at all.
	const FString Canon = FClaireonSessionManager::CanonicalizePath(
		TEXT("/Game/Test/BP_WithFlag"), /*bAllowUnsavedWorldPackage=*/true);
	UNTEST_EXPECT_TRUE(Canon == TEXT("/Game/Test/BP_WithFlag"));
	co_return;
}

#endif // WITH_UNTESTED
