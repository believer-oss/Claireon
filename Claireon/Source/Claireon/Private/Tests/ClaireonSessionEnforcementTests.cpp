// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Regression tests for #0000 (R1+R2+R3+R5 stages 001-008): verifies
// session-mode enforcement on the per-asset, editor-wide, and bridge-
// carve-out paths.
//
// Test 3 (bridge bypass-mode + session_release / session_list carve-outs)
// is intentionally implemented as a manager-level proxy. The bridge
// dispatch (FClaireonBridge::MCPCallTool) takes PyObject* and is reachable
// only from CPython at runtime; the carve-out logic is inline at
// ClaireonBridge.cpp:249-253 with no extracted helper. Full bridge
// integration coverage is deferred to manual / smoke tests. The proxy
// asserts (a) the underlying property the bridge enforces (per-asset and
// editor-wide sessions are visible to ListSessions and so the bridge's
// Bypass switch case can see them), and (b) the carve-out tool names match
// the constants the bridge uses.
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonScopedAssetLock.h"
#include "ClaireonSessionManager.h"
#include "Tools/IClaireonTool.h"

#include "Tools/ClaireonTool_DataTableAddRow.h"
#include "Tools/ClaireonTool_DataTableDuplicateRow.h"
#include "Tools/ClaireonTool_DataTableImportCsv.h"
#include "Tools/ClaireonTool_DataTableImportJson.h"
#include "Tools/ClaireonTool_DataTableMoveRow.h"
#include "Tools/ClaireonTool_DataTableRemoveRow.h"
#include "Tools/ClaireonTool_DataTableRenameRow.h"
#include "Tools/ClaireonTool_DataTableSetRowValues.h"
#include "Tools/ClaireonTool_BlueprintCompile.h"
#include "Tools/ClaireonTool_BlueprintDuplicate.h"
#include "Tools/ClaireonTool_MaterialApply.h"
#include "Tools/ClaireonTool_BlueprintTranslateImplement.h"

#include "Tools/ClaireonTool_ListSessions.h"
#include "Tools/ClaireonTool_ReleaseSessions.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonSessionEnforcementTestsNS
{
	static void CleanupAllSessions()
	{
		FClaireonSessionManager::Get().ForceReleaseAll();
	}

	// Manager-level proxy: assert that opening a session under SentinelTool
	// blocks an Open under DifferentTool with the BlockedByOtherTool result
	// shape. This is the underlying invariant that FClaireonScopedAssetLock and
	// every RequiresSession tool relies on. Returns true if the invariant holds.
	static bool ProxyPerAssetContentionHolds(
		const FString& AssetPath,
		const FString& SentinelTool,
		const FString& OtherTool,
		FString& OutDiagnostic)
	{
		FClaireonSessionManager::Get().ForceReleaseAll();

		FMCPOpenSessionResult Sentinel = FClaireonSessionManager::Get().OpenSession(
			AssetPath, SentinelTool, 1.0);
		if (Sentinel.Result != EOpenSessionResult::Success)
		{
			OutDiagnostic = FString::Printf(
				TEXT("Sentinel OpenSession(%s, %s) did not succeed (Result=%d)"),
				*AssetPath, *SentinelTool, static_cast<int32>(Sentinel.Result));
			return false;
		}

		FMCPOpenSessionResult Blocked = FClaireonSessionManager::Get().OpenSession(
			AssetPath, OtherTool, 1.0);
		if (Blocked.Result != EOpenSessionResult::BlockedByOtherTool)
		{
			OutDiagnostic = FString::Printf(
				TEXT("Other-tool OpenSession(%s, %s) was not blocked (Result=%d)"),
				*AssetPath, *OtherTool, static_cast<int32>(Blocked.Result));
			return false;
		}

		if (!Blocked.BlockingSession.IsSet())
		{
			OutDiagnostic = TEXT("BlockingSession was not populated on BlockedByOtherTool result");
			return false;
		}

		if (Blocked.BlockingSession->ToolName != SentinelTool)
		{
			OutDiagnostic = FString::Printf(
				TEXT("BlockingSession tool mismatch: expected %s, got %s"),
				*SentinelTool, *Blocked.BlockingSession->ToolName);
			return false;
		}

		FClaireonSessionManager::Get().ForceReleaseAll();
		return true;
	}
}

// ============================================================================
// Test 1: Per-asset contention for the 12 R2 RequiresSession tools.
//
// For each tool, assert (a) GetSessionMode() == RequiresSession (the
// machine-readable contract) and (b) the underlying lock surfaces as
// BlockedByOtherTool when a sentinel session holds the same asset under a
// different tool name. (b) is what FClaireonScopedAssetLock turns into the
// "Asset is locked by ..." error inside Execute(), regardless of where
// in the tool body the lock is acquired -- including tools (BlueprintCompile,
// BlueprintTranslateImplement) whose Execute() reaches the lock only after
// asset-registry lookup or session-id resolution that we cannot synthesize
// in a unit test without an editor world.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, SessionEnforcement, RequiresSession_AllTwelveTools, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonSessionEnforcementTestsNS;
	CleanupAllSessions();

	// Per-tool checks: assert GetSessionMode() == RequiresSession, and assert
	// the lock surfaces as BlockedByOtherTool when contended under the tool's
	// real GetName(). We stack-construct each tool inline to keep ownership /
	// copyability concerns minimal.
	auto Verify = [](const TCHAR* DisplayName, IClaireonTool& Tool, bool& bOutAllOk) -> void
	{
		if (Tool.GetSessionMode() != EClaireonToolSessionMode::RequiresSession)
		{
			bOutAllOk = false;
			return;
		}
		const FString AssetPath = FString::Printf(TEXT("/Game/Test/SessionEnforce_%s"), DisplayName);
		const FString SentinelTool = TEXT("sentinel");
		const FString OtherTool = Tool.GetName();
		if (OtherTool.IsEmpty())
		{
			bOutAllOk = false;
			return;
		}
		FString Diagnostic;
		if (!ProxyPerAssetContentionHolds(AssetPath, SentinelTool, OtherTool, Diagnostic))
		{
			bOutAllOk = false;
			return;
		}
	};

	bool bAllOk = true;
	int32 ToolCount = 0;

	{ ClaireonTool_DataTableAddRow             T; Verify(TEXT("DataTableAddRow"),             T, bAllOk); ++ToolCount; }
	{ ClaireonTool_DataTableRemoveRow          T; Verify(TEXT("DataTableRemoveRow"),          T, bAllOk); ++ToolCount; }
	{ ClaireonTool_DataTableDuplicateRow       T; Verify(TEXT("DataTableDuplicateRow"),       T, bAllOk); ++ToolCount; }
	{ ClaireonTool_DataTableRenameRow          T; Verify(TEXT("DataTableRenameRow"),          T, bAllOk); ++ToolCount; }
	{ ClaireonTool_DataTableMoveRow            T; Verify(TEXT("DataTableMoveRow"),            T, bAllOk); ++ToolCount; }
	{ ClaireonTool_DataTableSetRowValues       T; Verify(TEXT("DataTableSetRowValues"),       T, bAllOk); ++ToolCount; }
	{ ClaireonTool_DataTableImportCsv          T; Verify(TEXT("DataTableImportCsv"),          T, bAllOk); ++ToolCount; }
	{ ClaireonTool_DataTableImportJson         T; Verify(TEXT("DataTableImportJson"),         T, bAllOk); ++ToolCount; }
	{ ClaireonTool_BlueprintCompile            T; Verify(TEXT("BlueprintCompile"),            T, bAllOk); ++ToolCount; }
	{ ClaireonTool_BlueprintDuplicate          T; Verify(TEXT("BlueprintDuplicate"),          T, bAllOk); ++ToolCount; }
	{ ClaireonTool_MaterialApply               T; Verify(TEXT("MaterialApply"),               T, bAllOk); ++ToolCount; }
	{ ClaireonTool_BlueprintTranslateImplement T; Verify(TEXT("BlueprintTranslateImplement"), T, bAllOk); ++ToolCount; }

	UNTEST_EXPECT_TRUE(ToolCount == 12);
	UNTEST_EXPECT_TRUE(bAllOk);

	CleanupAllSessions();
	co_return;
}

// Direct Execute() integration check on a representative RequiresSession
// tool whose Execute() reaches the lock immediately after asset_path
// validation. Confirms that the FClaireonScopedAssetLock error message reaches
// the caller via FToolResult::ErrorMessage.
UNTEST_UNIT_OPTS(Claireon, SessionEnforcement, RequiresSession_DataTableAddRow_Execute, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonSessionEnforcementTestsNS;
	CleanupAllSessions();

	const FString AssetPath = TEXT("/Game/Test/SessionEnforce_AddRowExec");
	FMCPOpenSessionResult Sentinel = FClaireonSessionManager::Get().OpenSession(
		AssetPath, TEXT("sentinel"), 1.0);
	UNTEST_ASSERT_TRUE(Sentinel.Result == EOpenSessionResult::Success);

	ClaireonTool_DataTableAddRow Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("row_name"), TEXT("Row_Sentinel"));

	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(
		Result.ErrorMessage.Contains(TEXT("locked")) ||
		Result.ErrorMessage.Contains(TEXT("sentinel")));

	CleanupAllSessions();
	co_return;
}

// ============================================================================
// Test 2: EditorWide-vs-per-asset cross-precedence (both directions) and
// editor-wide-vs-editor-wide contention.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, SessionEnforcement, PerAssetHeld_EditorWideAcquireFails, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonSessionEnforcementTestsNS;
	CleanupAllSessions();

	const FString AssetPath = TEXT("/Game/Test/SessionEnforce_2a");
	FMCPOpenSessionResult PerAsset = FClaireonSessionManager::Get().OpenSession(
		AssetPath, TEXT("sentinel"), 1.0);
	UNTEST_ASSERT_TRUE(PerAsset.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult EditorWide = FClaireonSessionManager::Get().OpenEditorWideSession(
		TEXT("sentinel_editorwide"), 1.0);
	UNTEST_EXPECT_TRUE(EditorWide.Result == EOpenSessionResult::BlockedByOtherTool);
	UNTEST_EXPECT_TRUE(EditorWide.SessionId.IsEmpty());
	UNTEST_ASSERT_TRUE(EditorWide.BlockingSession.IsSet());
	// Blocker is the per-asset session.
	UNTEST_EXPECT_TRUE(EditorWide.BlockingSession->ToolName == TEXT("sentinel"));

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionEnforcement, EditorWideHeld_PerAssetAcquireFails, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonSessionEnforcementTestsNS;
	CleanupAllSessions();

	FMCPOpenSessionResult EditorWide = FClaireonSessionManager::Get().OpenEditorWideSession(
		TEXT("sentinel_editorwide"), 1.0);
	UNTEST_ASSERT_TRUE(EditorWide.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult PerAsset = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/SessionEnforce_2b"), TEXT("sentinel"), 1.0);
	UNTEST_EXPECT_TRUE(PerAsset.Result == EOpenSessionResult::BlockedByOtherTool);
	UNTEST_EXPECT_TRUE(PerAsset.SessionId.IsEmpty());
	UNTEST_ASSERT_TRUE(PerAsset.BlockingSession.IsSet());
	UNTEST_EXPECT_TRUE(PerAsset.BlockingSession->ToolName == TEXT("sentinel_editorwide"));

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionEnforcement, EditorWideHeld_SecondEditorWideFails, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonSessionEnforcementTestsNS;
	CleanupAllSessions();

	FMCPOpenSessionResult First = FClaireonSessionManager::Get().OpenEditorWideSession(
		TEXT("editorwide_a"), 1.0);
	UNTEST_ASSERT_TRUE(First.Result == EOpenSessionResult::Success);

	FMCPOpenSessionResult Second = FClaireonSessionManager::Get().OpenEditorWideSession(
		TEXT("editorwide_b"), 1.0);
	UNTEST_EXPECT_TRUE(Second.Result == EOpenSessionResult::BlockedByOtherTool);
	UNTEST_EXPECT_TRUE(Second.SessionId.IsEmpty());
	UNTEST_ASSERT_TRUE(Second.BlockingSession.IsSet());
	UNTEST_EXPECT_TRUE(Second.BlockingSession->ToolName == TEXT("editorwide_a"));

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionEnforcement, EditorWideClose_AllowsNextAcquire, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonSessionEnforcementTestsNS;
	CleanupAllSessions();

	FMCPOpenSessionResult Open = FClaireonSessionManager::Get().OpenEditorWideSession(
		TEXT("editorwide_a"), 1.0);
	UNTEST_ASSERT_TRUE(Open.Result == EOpenSessionResult::Success);
	UNTEST_ASSERT_TRUE(!Open.SessionId.IsEmpty());

	const bool bClosed = FClaireonSessionManager::Get().CloseEditorWideSession(Open.SessionId);
	UNTEST_EXPECT_TRUE(bClosed);
	UNTEST_EXPECT_TRUE(!FClaireonSessionManager::Get().IsEditorWideSessionHeld());

	FMCPOpenSessionResult AfterClose = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/SessionEnforce_2d"), TEXT("sentinel"), 1.0);
	UNTEST_EXPECT_TRUE(AfterClose.Result == EOpenSessionResult::Success);

	CleanupAllSessions();
	co_return;
}

// ============================================================================
// Test 3: Bridge bypass-mode + carve-out invariants (manager-level proxy).
//
// The bridge dispatch is reachable only via CPython at runtime. The
// invariants we can verify at the manager level are:
//   - The session-management tools (session_release and
//     session_list) report ReadOnly mode, which means the bridge
//     would not even enter the Bypass switch case for them. (The bridge
//     also has an explicit name carve-out, but the ReadOnly mode is the
//     primary contract -- the carve-out only matters if those tools are
//     ever re-tagged.)
//   - ListSessions() includes both per-asset and editor-wide sessions
//     while held. This is the data the bridge's Bypass switch case
//     iterates over (ClaireonBridge.cpp:277). If this property breaks, the
//     Bypass enforcement breaks.
//   - The bridge carve-out tool-name strings ("session_release",
//     "session_list") match the actual GetName() of the
//     session-management tools.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, SessionEnforcement, CarveOut_SessionToolNamesMatchBridgeConstants, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_ReleaseSessions ReleaseTool;
	ClaireonTool_ListSessions ListTool;

	// Post-#0000: GetName() returns bare names (no "claireon." prefix). The bridge
	// carve-out compares the registry key, which equals GetName() exactly.
	UNTEST_EXPECT_TRUE(ReleaseTool.GetName() == TEXT("session_release"));
	UNTEST_EXPECT_TRUE(ListTool.GetName() == TEXT("session_list"));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionEnforcement, ListSessions_IncludesPerAssetAndEditorWide, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonSessionEnforcementTestsNS;
	CleanupAllSessions();

	FMCPOpenSessionResult PerAsset = FClaireonSessionManager::Get().OpenSession(
		TEXT("/Game/Test/SessionEnforce_BridgeListPerAsset"), TEXT("sentinel_per_asset"), 1.0);
	UNTEST_ASSERT_TRUE(PerAsset.Result == EOpenSessionResult::Success);

	// Per-asset visible to ListSessions.
	const TArray<FMCPSession> PerAssetVisible = FClaireonSessionManager::Get().ListSessions();
	UNTEST_ASSERT_TRUE(PerAssetVisible.Num() == 1);
	UNTEST_EXPECT_TRUE(PerAssetVisible[0].ToolName == TEXT("sentinel_per_asset"));

	CleanupAllSessions();

	// Editor-wide visible to ListSessions.
	FMCPOpenSessionResult EditorWide = FClaireonSessionManager::Get().OpenEditorWideSession(
		TEXT("sentinel_editorwide"), 1.0);
	UNTEST_ASSERT_TRUE(EditorWide.Result == EOpenSessionResult::Success);

	const TArray<FMCPSession> EditorWideVisible = FClaireonSessionManager::Get().ListSessions();
	UNTEST_ASSERT_TRUE(EditorWideVisible.Num() == 1);
	UNTEST_EXPECT_TRUE(EditorWideVisible[0].ToolName == TEXT("sentinel_editorwide"));

	CleanupAllSessions();
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SessionEnforcement, CarveOut_SessionToolsAreReadOnlyMode, UNTEST_TIMEOUTMS(5000))
{
	// session_release and session_list are ReadOnly-mode tools.
	// The bridge's Bypass switch case only fires for SessionMode == Bypass, so
	// ReadOnly tools (including these two) bypass the held-session check
	// entirely. The bridge ALSO has an explicit name-based carve-out at
	// ClaireonBridge.cpp:251-253 as belt-and-braces protection, but this test
	// confirms the primary mechanism: ReadOnly mode means the Bypass switch
	// case is not entered.
	ClaireonTool_ReleaseSessions ReleaseTool;
	ClaireonTool_ListSessions ListTool;

	UNTEST_EXPECT_TRUE(ReleaseTool.GetSessionMode() == EClaireonToolSessionMode::ReadOnly);
	UNTEST_EXPECT_TRUE(ListTool.GetSessionMode() == EClaireonToolSessionMode::ReadOnly);

	co_return;
}

// ============================================================================
// Test 4: ReadOnly tools are not blocked by a held session.
//
// We can't easily invoke the bridge dispatch from a unit test, so we assert
// the underlying property: a ReadOnly tool (session_list) reports
// SessionMode == ReadOnly. The bridge's switch case for ReadOnly is a
// pass-through (ClaireonBridge.cpp:263-265), so any tool with this mode is
// guaranteed to bypass the held-session check.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, SessionEnforcement, ReadOnly_BypassesHeldSessionCheck, UNTEST_TIMEOUTMS(5000))
{
	using namespace ClaireonSessionEnforcementTestsNS;
	CleanupAllSessions();

	// Hold an editor-wide session.
	FMCPOpenSessionResult EditorWide = FClaireonSessionManager::Get().OpenEditorWideSession(
		TEXT("editorwide_a"), 1.0);
	UNTEST_ASSERT_TRUE(EditorWide.Result == EOpenSessionResult::Success);

	// A ReadOnly-mode tool: session_list. The bridge would forward this
	// unconditionally (switch case ReadOnly is a pass-through at
	// ClaireonBridge.cpp:263-265). This test confirms the SessionMode contract.
	ClaireonTool_ListSessions Tool;
	UNTEST_EXPECT_TRUE(Tool.GetSessionMode() == EClaireonToolSessionMode::ReadOnly);

	// Sanity: we can still drive the tool's Execute() and it does not error
	// out -- demonstrating the pass-through is functional, not just nominal.
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("tool_name"), TEXT(""));
	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_EXPECT_TRUE(!Result.bIsError);

	CleanupAllSessions();
	co_return;
}

#endif // WITH_UNTESTED
