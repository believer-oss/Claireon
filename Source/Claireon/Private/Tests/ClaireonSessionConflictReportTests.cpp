// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonSessionManager.h"

// WI-16: session-conflict errors must name EVERY blocker at once (previously
// they named one blocker per attempt, costing N retries for N sessions).
//
// These declarations bind to definitions in ClaireonBridge.cpp that carry
// external linkage specifically so this test can exercise the exact wire text
// without going through the Python bridge entry point.
extern FString ClaireonBridge_FormatSessionBlockedError(
	const FString& ToolName,
	const FMCPSession& FirstBlocker,
	const TArray<FMCPSession>& AllBlockers);
extern TArray<FMCPSession> ClaireonBridge_CollectBypassBlockers(
	const FString& ToolName,
	const TArray<FMCPSession>& HeldSessions);

// ---------------------------------------------------------------------------
// Helpers (SCRT_ prefix = file-local discriminator for unity batching)
// ---------------------------------------------------------------------------

namespace ClaireonSessionConflictReportTest
{
	static void SCRT_CleanupAllSessions()
	{
		FClaireonSessionManager::Get().ForceReleaseAll();
	}

	static FMCPSession SCRT_MakeSession(const FString& SessionId, const FString& ToolName, const FString& AssetPath)
	{
		FMCPSession Session;
		Session.SessionId = SessionId;
		Session.ToolName = ToolName;
		Session.AssetPath = AssetPath;
		Session.CreatedTime = FDateTime::UtcNow();
		Session.LastAccessTime = FDateTime::UtcNow();
		Session.TimeoutMinutes = 60.0;
		return Session;
	}

	static int32 SCRT_CountOccurrences(const FString& Haystack, const FString& Needle)
	{
		int32 Count = 0;
		int32 Pos = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, 0);
		while (Pos != INDEX_NONE)
		{
			++Count;
			Pos = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos + Needle.Len());
		}
		return Count;
	}

	static FString SCRT_FirstLine(const FString& Message)
	{
		int32 NewlinePos = INDEX_NONE;
		if (Message.FindChar(TEXT('\n'), NewlinePos))
		{
			return Message.Left(NewlinePos);
		}
		return Message;
	}
} // namespace ClaireonSessionConflictReportTest

// ============================================================================
// SessionConflictReport Tests
// ============================================================================

// The doc's required scenario: three per-asset sessions held, an editor-wide
// attempt is blocked, and the composed error names all three ids (and asset
// paths). Closing all three and retrying succeeds.
UNTEST_UNIT_OPTS(Claireon, SessionConflictReport, EditorWideBlockedListsAllBlockers, UNTEST_TIMEOUTMS(5000))
{
	ClaireonSessionConflictReportTest::SCRT_CleanupAllSessions();

	FMCPOpenSessionResult OpenA = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_ConflictReportA"), TEXT("test_edit_a"));
	UNTEST_ASSERT_TRUE(OpenA.Result == EOpenSessionResult::Success);
	FMCPOpenSessionResult OpenB = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_ConflictReportB"), TEXT("test_edit_b"));
	UNTEST_ASSERT_TRUE(OpenB.Result == EOpenSessionResult::Success);
	FMCPOpenSessionResult OpenC = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/BP_ConflictReportC"), TEXT("test_edit_c"));
	UNTEST_ASSERT_TRUE(OpenC.Result == EOpenSessionResult::Success);

	const FString EditorWideTool = TEXT("conflict_report_editorwide");
	FMCPOpenSessionResult EditorWide = FClaireonSessionManager::Get().OpenEditorWideSession(EditorWideTool);
	UNTEST_ASSERT_TRUE(EditorWide.Result == EOpenSessionResult::BlockedByOtherTool);
	UNTEST_ASSERT_TRUE(EditorWide.BlockingSession.IsSet());

	// Compose the error exactly the way FClaireonBridge::MCPCallTool does for
	// EditorWide-mode tools: first blocker from the open result, full set from
	// ListSessions().
	const TArray<FMCPSession> AllHeld = FClaireonSessionManager::Get().ListSessions();
	UNTEST_ASSERT_TRUE(AllHeld.Num() == 3);
	const FString Error = ClaireonBridge_FormatSessionBlockedError(
		EditorWideTool, EditorWide.BlockingSession.GetValue(), AllHeld);

	// ONE error names every blocking session id and asset path.
	UNTEST_EXPECT_TRUE(Error.Contains(OpenA.SessionId, ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(Error.Contains(OpenB.SessionId, ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(Error.Contains(OpenC.SessionId, ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("/Game/Test/BP_ConflictReportA"), ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("/Game/Test/BP_ConflictReportB"), ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("/Game/Test/BP_ConflictReportC"), ESearchCase::CaseSensitive));

	// One bullet per blocker (all three, first blocker included).
	UNTEST_EXPECT_TRUE(ClaireonSessionConflictReportTest::SCRT_CountOccurrences(Error, TEXT("\n- ")) == 3);

	// First line stays backward-compatible with the legacy single-blocker
	// format that existing retry logic parses.
	const FString FirstBlockerId = EditorWide.BlockingSession->SessionId;
	const FString FirstLine = ClaireonSessionConflictReportTest::SCRT_FirstLine(Error);
	UNTEST_EXPECT_TRUE(FirstLine.StartsWith(TEXT("Tool 'conflict_report_editorwide' blocked:"), ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(FirstLine.Contains(TEXT("holds the lock"), ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(FirstLine.Contains(
		FString::Printf(TEXT("session_release with session_id='%s'"), *FirstBlockerId), ESearchCase::CaseSensitive));

	// Close all three in a single pass, retry -> success.
	UNTEST_EXPECT_TRUE(FClaireonSessionManager::Get().CloseSession(OpenA.SessionId));
	UNTEST_EXPECT_TRUE(FClaireonSessionManager::Get().CloseSession(OpenB.SessionId));
	UNTEST_EXPECT_TRUE(FClaireonSessionManager::Get().CloseSession(OpenC.SessionId));

	FMCPOpenSessionResult Retry = FClaireonSessionManager::Get().OpenEditorWideSession(EditorWideTool);
	UNTEST_ASSERT_TRUE(Retry.Result == EOpenSessionResult::Success);
	UNTEST_EXPECT_TRUE(FClaireonSessionManager::Get().CloseEditorWideSession(Retry.SessionId));

	ClaireonSessionConflictReportTest::SCRT_CleanupAllSessions();
	co_return;
}

// A single blocker keeps the legacy one-line message verbatim (no bullet list,
// no trailing newline) so existing single-session retry parsing is unchanged.
UNTEST_UNIT_OPTS(Claireon, SessionConflictReport, SingleBlockerKeepsLegacySingleLine, UNTEST_TIMEOUTMS(5000))
{
	const FMCPSession Blocker = ClaireonSessionConflictReportTest::SCRT_MakeSession(
		TEXT("11111111"), TEXT("bp"), TEXT("/Game/Test/BP_Solo"));

	TArray<FMCPSession> AllBlockers;
	AllBlockers.Add(Blocker);

	const FString Error = ClaireonBridge_FormatSessionBlockedError(TEXT("some_tool"), Blocker, AllBlockers);

	int32 NewlinePos = INDEX_NONE;
	UNTEST_EXPECT_TRUE(!Error.FindChar(TEXT('\n'), NewlinePos));
	UNTEST_EXPECT_TRUE(Error.StartsWith(TEXT("Tool 'some_tool' blocked:"), ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("bp session 11111111 holds the lock"), ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("session_release with session_id='11111111'"), ESearchCase::CaseSensitive));
	co_return;
}

// Multiple blockers: the first blocker owns the (legacy) first line only, and
// the bullet list names every blocker exactly once, deduplicating the first
// blocker's entry in AllBlockers.
UNTEST_UNIT_OPTS(Claireon, SessionConflictReport, MultiBlockerNamesEveryIdAndPathOnce, UNTEST_TIMEOUTMS(5000))
{
	const FMCPSession Blocker1 = ClaireonSessionConflictReportTest::SCRT_MakeSession(
		TEXT("11111111"), TEXT("bp"), TEXT("/Game/Test/BP_One"));
	const FMCPSession Blocker2 = ClaireonSessionConflictReportTest::SCRT_MakeSession(
		TEXT("22222222"), TEXT("niagara_edit"), TEXT("/Game/Test/NS_Two"));
	const FMCPSession Blocker3 = ClaireonSessionConflictReportTest::SCRT_MakeSession(
		TEXT("33333333"), TEXT("statetree_edit"), TEXT("/Game/Test/ST_Three"));

	TArray<FMCPSession> AllBlockers;
	AllBlockers.Add(Blocker1); // duplicate of FirstBlocker -- must be deduped
	AllBlockers.Add(Blocker2);
	AllBlockers.Add(Blocker3);

	const FString Error = ClaireonBridge_FormatSessionBlockedError(TEXT("some_tool"), Blocker1, AllBlockers);

	// Exactly three bullets, one per unique blocker.
	UNTEST_EXPECT_TRUE(ClaireonSessionConflictReportTest::SCRT_CountOccurrences(Error, TEXT("\n- ")) == 3);
	UNTEST_EXPECT_TRUE(ClaireonSessionConflictReportTest::SCRT_CountOccurrences(
		Error, TEXT("- bp session 11111111 on '/Game/Test/BP_One'")) == 1);
	UNTEST_EXPECT_TRUE(ClaireonSessionConflictReportTest::SCRT_CountOccurrences(
		Error, TEXT("- niagara_edit session 22222222 on '/Game/Test/NS_Two'")) == 1);
	UNTEST_EXPECT_TRUE(ClaireonSessionConflictReportTest::SCRT_CountOccurrences(
		Error, TEXT("- statetree_edit session 33333333 on '/Game/Test/ST_Three'")) == 1);

	// The first line names only the first blocker.
	const FString FirstLine = ClaireonSessionConflictReportTest::SCRT_FirstLine(Error);
	UNTEST_EXPECT_TRUE(FirstLine.Contains(TEXT("11111111"), ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(!FirstLine.Contains(TEXT("22222222"), ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(!FirstLine.Contains(TEXT("33333333"), ESearchCase::CaseSensitive));
	co_return;
}

// Bypass-mode blocker collection: every session held by a DIFFERENT tool is a
// blocker; same-tool sessions never block; the composed error names all of
// the conflicting ids in one message.
UNTEST_UNIT_OPTS(Claireon, SessionConflictReport, BypassCollectsEveryConflictingSession, UNTEST_TIMEOUTMS(5000))
{
	TArray<FMCPSession> Held;
	Held.Add(ClaireonSessionConflictReportTest::SCRT_MakeSession(
		TEXT("11111111"), TEXT("same_tool"), TEXT("/Game/Test/BP_Mine")));
	Held.Add(ClaireonSessionConflictReportTest::SCRT_MakeSession(
		TEXT("22222222"), TEXT("other_tool_a"), TEXT("/Game/Test/BP_OtherA")));
	Held.Add(ClaireonSessionConflictReportTest::SCRT_MakeSession(
		TEXT("33333333"), TEXT("other_tool_b"), TEXT("/Game/Test/BP_OtherB")));

	const TArray<FMCPSession> Blockers = ClaireonBridge_CollectBypassBlockers(TEXT("same_tool"), Held);
	UNTEST_ASSERT_TRUE(Blockers.Num() == 2);
	UNTEST_EXPECT_TRUE(Blockers[0].SessionId == TEXT("22222222"));
	UNTEST_EXPECT_TRUE(Blockers[1].SessionId == TEXT("33333333"));

	const FString Error = ClaireonBridge_FormatSessionBlockedError(TEXT("same_tool"), Blockers[0], Blockers);
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("22222222"), ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("33333333"), ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(!Error.Contains(TEXT("11111111"), ESearchCase::CaseSensitive));

	// All-same-tool: nothing blocks.
	TArray<FMCPSession> HeldSameOnly;
	HeldSameOnly.Add(ClaireonSessionConflictReportTest::SCRT_MakeSession(
		TEXT("44444444"), TEXT("same_tool"), TEXT("/Game/Test/BP_Mine2")));
	const TArray<FMCPSession> NoBlockers = ClaireonBridge_CollectBypassBlockers(TEXT("same_tool"), HeldSameOnly);
	UNTEST_EXPECT_TRUE(NoBlockers.Num() == 0);
	co_return;
}

#endif // WITH_UNTESTED
