// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonSessionManager.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void CleanupAllSessions()
{
	FClaireonSessionManager::Get().ForceReleaseAll();
}

// ============================================================================
// SessionManager Tests
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, SessionManager, OpenCloseLifecycle, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_TestAsset"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(OpenResult.Result == EOpenSessionResult::Success);
	UNTEST_EXPECT_TRUE(!OpenResult.SessionId.IsEmpty());

	FMCPSession* Found = FClaireonSessionManager::Get().FindSession(OpenResult.SessionId);
	UNTEST_ASSERT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(Found->ToolName == TEXT("editor.test.edit"));
	UNTEST_EXPECT_TRUE(Found->AssetPath == TEXT("/Game/Test/BP_TestAsset"));

	bool bClosed = FClaireonSessionManager::Get().CloseSession(OpenResult.SessionId);
	UNTEST_EXPECT_TRUE(bClosed);

	FMCPSession* AfterClose = FClaireonSessionManager::Get().FindSession(OpenResult.SessionId);
	UNTEST_EXPECT_TRUE(AfterClose == nullptr);

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, SameToolReopen, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Reopen"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Reopen"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);
	UNTEST_EXPECT_TRUE(Second.SessionId == First.SessionId);

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, DifferentToolBlock, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Block"), TEXT("editor.toolA.edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Block"), TEXT("editor.toolB.edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::BlockedByOtherTool);
	UNTEST_EXPECT_TRUE(Second.SessionId.IsEmpty());
	UNTEST_EXPECT_TRUE(Second.BlockingSession.IsSet());
	UNTEST_EXPECT_TRUE(Second.BlockingSession->ToolName == TEXT("editor.toolA.edit"));

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, PathCanonicalization, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Foo.uasset"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Foo"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);
	UNTEST_EXPECT_TRUE(Second.SessionId == First.SessionId);

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, PathCanonObjectNameSuffix, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Foo.BP_Foo"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Foo"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);
	UNTEST_EXPECT_TRUE(Second.SessionId == First.SessionId);

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, InvalidPathRejection, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	FMCPOpenSessionResult Result = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Script/Engine.Actor"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(Result.Result == EOpenSessionResult::InvalidAssetPath);
	UNTEST_EXPECT_TRUE(Result.SessionId.IsEmpty());

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, IsAssetLocked, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_LockTest"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(OpenResult.Result == EOpenSessionResult::Success);

	bool bLocked = FClaireonSessionManager::Get().IsAssetLocked(TEXT("/Game/Test/BP_LockTest"));
	UNTEST_EXPECT_TRUE(bLocked);

	FClaireonSessionManager::Get().CloseSession(OpenResult.SessionId);

	bool bLockedAfter = FClaireonSessionManager::Get().IsAssetLocked(TEXT("/Game/Test/BP_LockTest"));
	UNTEST_EXPECT_TRUE(!bLockedAfter);

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, ListSessions, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	FClaireonSessionManager::Get().OpenSession(TEXT("/Game/Test/BP_A"), TEXT("editor.blueprint.edit"));
	FClaireonSessionManager::Get().OpenSession(TEXT("/Game/Test/BP_B"), TEXT("editor.niagara.edit"));

	TArray<FMCPSession> All = FClaireonSessionManager::Get().ListSessions();
	UNTEST_EXPECT_TRUE(All.Num() == 2);

	TArray<FMCPSession> BPOnly = FClaireonSessionManager::Get().ListSessions(TEXT("editor.blueprint.edit"));
	UNTEST_EXPECT_TRUE(BPOnly.Num() == 1);

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, ReleaseByAssetPath, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Release"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(OpenResult.Result == EOpenSessionResult::Success);

	int32 Released = FClaireonSessionManager::Get().ReleaseByAssetPath(TEXT("/Game/Test/BP_Release"));
	UNTEST_EXPECT_TRUE(Released == 1);

	FMCPSession* AfterRelease = FClaireonSessionManager::Get().FindSession(OpenResult.SessionId);
	UNTEST_EXPECT_TRUE(AfterRelease == nullptr);

	int32 ReleasedAgain = FClaireonSessionManager::Get().ReleaseByAssetPath(TEXT("/Game/Test/BP_Release"));
	UNTEST_EXPECT_TRUE(ReleasedAgain == 0);

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, ForceReleaseAll, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	FClaireonSessionManager::Get().OpenSession(TEXT("/Game/Test/BP_1"), TEXT("editor.test.edit"));
	FClaireonSessionManager::Get().OpenSession(TEXT("/Game/Test/BP_2"), TEXT("editor.test.edit"));
	FClaireonSessionManager::Get().OpenSession(TEXT("/Game/Test/BP_3"), TEXT("editor.test.edit"));

	int32 Count = FClaireonSessionManager::Get().ForceReleaseAll();
	UNTEST_EXPECT_TRUE(Count == 3);

	TArray<FMCPSession> Remaining = FClaireonSessionManager::Get().ListSessions();
	UNTEST_EXPECT_TRUE(Remaining.Num() == 0);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, Expiry, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	// Open with short timeout
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Expiry"), TEXT("editor.test.edit"), 30.0);
	UNTEST_ASSERT_TRUE(OpenResult.Result == EOpenSessionResult::Success);

	// Get the session pointer and manually set LastAccessTime far in the past
	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(OpenResult.SessionId);
	UNTEST_ASSERT_TRUE(Session != nullptr);
	Session->LastAccessTime = FDateTime::UtcNow() - FTimespan::FromMinutes(60.0);

	// Now FindSession should detect the session as expired and remove it
	FMCPSession* Found = FClaireonSessionManager::Get().FindSession(OpenResult.SessionId);
	UNTEST_EXPECT_TRUE(Found == nullptr);

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, CloseSessionNoOp, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	bool bResult = FClaireonSessionManager::Get().CloseSession(TEXT("nonexistent-session-id"));
	UNTEST_EXPECT_TRUE(!bResult);

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, DelegateFiresOnClose, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

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
		TEXT("/Game/Test/BP_Delegate"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(OpenResult.Result == EOpenSessionResult::Success);

	FClaireonSessionManager::Get().CloseSession(OpenResult.SessionId);

	UNTEST_EXPECT_TRUE(bDelegateFired);
	UNTEST_EXPECT_TRUE(CapturedSessionId == OpenResult.SessionId);
	UNTEST_EXPECT_TRUE(CapturedAssetPath == TEXT("/Game/Test/BP_Delegate"));
	UNTEST_EXPECT_TRUE(CapturedToolName == TEXT("editor.test.edit"));

	FClaireonSessionManager::Get().OnSessionClosed().Remove(Handle);
	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, DelegateDoesNotFireOnNoOp, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	bool bDelegateFired = false;

	FDelegateHandle Handle = FClaireonSessionManager::Get().OnSessionClosed().AddLambda(
		[&](const FMCPSessionClosedInfo& Info)
	{
		bDelegateFired = true;
	});

	FClaireonSessionManager::Get().CloseSession(TEXT("nonexistent-session"));
	UNTEST_EXPECT_TRUE(!bDelegateFired);

	FClaireonSessionManager::Get().OnSessionClosed().Remove(Handle);
	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, TimeoutMaxOnReopen, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Timeout"), TEXT("editor.test.edit"), 10.0);
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Timeout"), TEXT("editor.test.edit"), 60.0);
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);

	FMCPSession* Found = FClaireonSessionManager::Get().FindSession(First.SessionId);
	UNTEST_ASSERT_TRUE(Found != nullptr);
	UNTEST_EXPECT_TRUE(FMath::IsNearlyEqual(Found->TimeoutMinutes, 60.0));

	CleanupAllSessions();
	co_return;
}

// ---------------------------------------------------------------------------
// Path canonicalization: double slashes, backslashes, trailing slash
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, SessionManager, PathCanonDoubleSlashes, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	// Double slashes should be collapsed to single slashes
	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game//Test//BP_DoubleSlash"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	// Normal path should reuse the same session
	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_DoubleSlash"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);
	UNTEST_EXPECT_TRUE(Second.SessionId == First.SessionId);

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, PathCanonBackslashes, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	// Backslashes should be normalized to forward slashes
	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game\\Test\\BP_Backslash"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Backslash"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);
	UNTEST_EXPECT_TRUE(Second.SessionId == First.SessionId);

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionManager, PathCanonTrailingSlash, UNTEST_TIMEOUTMS(5000))
{
	CleanupAllSessions();

	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Trailing/"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_Trailing"), TEXT("editor.test.edit"));
	UNTEST_ASSERT_TRUE(Second.Result == EOpenSessionResult::ReusedExistingSession);
	UNTEST_EXPECT_TRUE(Second.SessionId == First.SessionId);

	CleanupAllSessions();
	co_return;
}


#endif // WITH_UNTESTED
