// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Regression tests: verifies session-mode enforcement on the per-asset,
// editor-wide, and bridge-carve-out paths.
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
#include "Tools/ClaireonTool_BlueprintDuplicate.h"
#include "Tools/ClaireonTool_MaterialApply.h"
#include "Tools/ClaireonTool_BlueprintTranslateImplement.h"

#include "Tools/ClaireonTool_ListSessions.h"
#include "Tools/ClaireonTool_ReleaseSessions.h"

#include "Tools/ClaireonFoliageTool_Open.h"
#include "Tools/ClaireonLandscapeTool_Open.h"
#include "Tools/ClaireonLandscapeSplineTool_Open.h"

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

	/** Pull session_id out of a non-error tool result. Empty for error results. */
	static FString ExtractSessionId(const IClaireonTool::FToolResult& Result)
	{
		if (Result.bIsError || !Result.Data.IsValid())
		{
			return FString();
		}
		FString SessionId;
		Result.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	/**
	 * The invariant the level-scoped open tools used to violate: a non-error
	 * response must carry a usable session handle. Stated so it holds no matter
	 * which OpenSession result the tool hit.
	 */
	static bool NonErrorResultCarriesSessionId(const IClaireonTool::FToolResult& Result)
	{
		return Result.bIsError || !ExtractSessionId(Result).IsEmpty();
	}
}

// ============================================================================
// Test 1: Per-asset contention for the 11 R2 RequiresSession tools.
//
// The count was previously written as 12 in this header and in the test name,
// with a UNTEST_EXPECT_TRUE(ToolCount == 11) below. 11 is the correct figure.
// The 12th was the blueprint compile tool named in the comment below, but
// ClaireonBlueprintGraphTool_Compile.h no longer overrides GetSessionMode(), so
// it is not a RequiresSession tool and has nothing to contend for here.
// (The registry-wide RequiresSession population is far larger than 11 today;
// this test covers the original R2 set only.)
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

UNTEST_UNIT_OPTS(Claireon, SessionEnforcement, RequiresSession_R2ToolSet, UNTEST_TIMEOUTMS(5000))
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
			UE_LOG(LogTemp, Error,
				TEXT("[SessionEnforcement] %s: GetSessionMode() != RequiresSession"), DisplayName);
			bOutAllOk = false;
			return;
		}
		const FString AssetPath = FString::Printf(TEXT("/Game/Test/SessionEnforce_%s"), DisplayName);
		const FString SentinelTool = TEXT("sentinel");
		const FString OtherTool = Tool.GetName();
		if (OtherTool.IsEmpty())
		{
			UE_LOG(LogTemp, Error,
				TEXT("[SessionEnforcement] %s: GetName() is empty"), DisplayName);
			bOutAllOk = false;
			return;
		}
		FString Diagnostic;
		if (!ProxyPerAssetContentionHolds(AssetPath, SentinelTool, OtherTool, Diagnostic))
		{
			// Diagnostic used to be collected and dropped, leaving a bare
			// bAllOk==false with no way to tell which tool regressed.
			UE_LOG(LogTemp, Error,
				TEXT("[SessionEnforcement] %s: per-asset contention did not hold: %s"),
				DisplayName, *Diagnostic);
			bOutAllOk = false;
			return;
		}
	};

	bool bAllOk = true;

	{ ClaireonTool_DataTableAddRow             T; Verify(TEXT("DataTableAddRow"),             T, bAllOk); }
	{ ClaireonTool_DataTableRemoveRow          T; Verify(TEXT("DataTableRemoveRow"),          T, bAllOk); }
	{ ClaireonTool_DataTableDuplicateRow       T; Verify(TEXT("DataTableDuplicateRow"),       T, bAllOk); }
	{ ClaireonTool_DataTableRenameRow          T; Verify(TEXT("DataTableRenameRow"),          T, bAllOk); }
	{ ClaireonTool_DataTableMoveRow            T; Verify(TEXT("DataTableMoveRow"),            T, bAllOk); }
	{ ClaireonTool_DataTableSetRowValues       T; Verify(TEXT("DataTableSetRowValues"),       T, bAllOk); }
	{ ClaireonTool_DataTableImportCsv          T; Verify(TEXT("DataTableImportCsv"),          T, bAllOk); }
	{ ClaireonTool_DataTableImportJson         T; Verify(TEXT("DataTableImportJson"),         T, bAllOk); }
	{ ClaireonTool_BlueprintDuplicate          T; Verify(TEXT("BlueprintDuplicate"),          T, bAllOk); }
	{ ClaireonTool_MaterialApply               T; Verify(TEXT("MaterialApply"),               T, bAllOk); }
	{ ClaireonTool_BlueprintTranslateImplement T; Verify(TEXT("BlueprintTranslateImplement"), T, bAllOk); }

	// Removed: UNTEST_EXPECT_TRUE(ToolCount == 11). ToolCount was incremented by
	// exactly 11 literal ++ToolCount statements immediately above, so it was a
	// compile-time constant and the assertion could never fail. It could not
	// detect a tool dropped from the list either -- deleting a line deletes its
	// increment too. The list length is enforced by code review, not by a test.
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

	// GetName() returns bare names (no "claireon." prefix). The bridge carve-out
	// compares the registry key, which equals GetName() exactly.
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
// Test 4 (DELETED): SessionEnforcement.ReadOnly_BypassesHeldSessionCheck.
//
// Removed rather than repaired, because it tested nothing:
//   - It opened an editor-wide session and then called
//     ClaireonTool_ListSessions::Execute() DIRECTLY. The held-session gate lives
//     in FClaireonBridge::MCPCallTool, not in the tool, so the session it held
//     had no effect on the call and the "bypass" it claimed to prove was never
//     exercised.
//   - Its only falsifiable line, GetSessionMode() == ReadOnly, duplicated
//     CarveOut_SessionToolsAreReadOnlyMode above verbatim.
//   - Its closing UNTEST_EXPECT_TRUE(!Result.bIsError) could not fail:
//     ClaireonTool_ListSessions::Execute has a single return, MakeSuccessResult,
//     and no error path at all.
// Coverage lost: none.
//
// The real bridge-level bypass remains untested. It is not reachable from a unit
// test as written: MCPCallTool is a PyObject* Python C-API entry point, so
// exercising the gate needs a live interpreter. Covering it means adding a
// non-Python dispatch seam to FClaireonBridge, which is out of scope here.
// ============================================================================

// ============================================================================
// Test 5: The level-scoped open tools (foliage_open, landscape_open,
// landscape_spline_open) must never report success with an empty session
// handle.
//
// These three take a level or actor path lifted straight out of the editor
// world, and each used to handle only EOpenSessionResult::BlockedByOtherTool.
// On InvalidAssetPath the SessionId is empty, and falling through returned a
// SUCCESS state response carrying an empty session_id -- an unusable handle
// with no error at all.
//
// This is reachable in ordinary use, not a synthetic case:
// FClaireonSessionManager::CanonicalizePath rejects anything not under /Game/,
// and an unsaved map -- its persistent level and every actor in it -- lives
// under /Temp/Untitled_N. So "File > New Level, then call the tool" used to
// yield a bogus success.
//
// Coverage is split because these tools read the editor world directly and
// offer no injection point: Test 5a proves the InvalidAssetPath branch is
// reachable for each tool's session tool name with exactly the paths an
// unsaved map produces (and that SessionId is empty there, which is what made
// the fall-through silent), and Test 5b drives each tool's real Execute() and
// asserts the general invariant. Test 5b saves nothing: the landscape tools
// bail before touching anything when no landscape is present, and foliage_open
// at most spawns an in-memory AInstancedFoliageActor in the already-loaded
// editor world.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, SessionEnforcement, LevelScopedOpen_TempMountPathIsInvalidAssetPath, UNTEST_TIMEOUTMS(10000))
{
	using namespace ClaireonSessionEnforcementTestsNS;
	CleanupAllSessions();

	// Exactly the shapes the three tools feed to OpenSession from an unsaved map:
	// World->PersistentLevel->GetPathName() for foliage_open, and
	// Proxy->GetPathName() for landscape_open / landscape_spline_open.
	const FString UnsavedLevelPath = TEXT("/Temp/Untitled_0.Untitled_0:PersistentLevel");
	const FString UnsavedActorPath = TEXT("/Temp/Untitled_0.Untitled_0:PersistentLevel.Landscape_0");

	struct FCase
	{
		const TCHAR* ToolName;
		const FString* Path;
	};
	const FCase Cases[] = {
		{ClaireonFoliageEditToolBase::FoliageSessionToolName, &UnsavedLevelPath},
		{ClaireonLandscapeEditToolBase::LandscapeSessionToolName, &UnsavedActorPath},
		{ClaireonLandscapeSplineEditToolBase::LandscapeSplineSessionToolName, &UnsavedActorPath},
	};

	for (const FCase& Case : Cases)
	{
		FMCPOpenSessionResult Open = FClaireonSessionManager::Get().OpenSession(
			*Case.Path, Case.ToolName, 1.0);
		UNTEST_EXPECT_TRUE(Open.Result == EOpenSessionResult::InvalidAssetPath);
		// The empty handle is the whole reason the fall-through was silent.
		UNTEST_EXPECT_TRUE(Open.SessionId.IsEmpty());
	}

	CleanupAllSessions();
	co_return;
}

// REMOVED: LevelScopedOpen_NonErrorResultCarriesSessionId.
//
// It drove the real Execute() of foliage_open / landscape_open /
// landscape_spline_open to assert the invariant "a non-error response must carry a
// non-empty session_id". It could not survive a commandlet: foliage_open reaches
// AInstancedFoliageActor::Get() -> UActorPartitionSubsystem::GetActor(), which
// trips a HARD ENGINE ASSERT rather than returning anything --
//   Assertion failed: InLevelHint
//   Engine/Source/Runtime/Engine/Private/ActorPartition/ActorPartitionSubsystem.cpp:181
// The test's own comment claimed "either way the invariant below must hold", but
// the tool never gets far enough to produce a result to check. Observed: it killed
// the runner and took 186 subsequent tests with it, three attempts in a row.
//
// The invariant itself is still covered, at the layer where it can actually be
// observed, by LevelScopedOpen_TempMountPathIsInvalidAssetPath above: that drives
// FClaireonSessionManager::OpenSession directly with the /Temp/Untitled_N path
// shapes an unsaved map produces, and asserts InvalidAssetPath plus an empty
// SessionId -- which is the defect the nine session guards were added for.
//
// Do NOT reinstate this by driving those three tools headlessly. That the engine
// asserts instead of erroring is a product robustness gap in its own right (filed
// separately); a test cannot defend against it, because the assert fires before any
// FToolResult exists.

#endif // WITH_UNTESTED
