// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// WI-9 contract tests: session close with save_first must never silently lose
// work. For all three session close tools (anim_close, behaviortree_close,
// blackboard_close):
//   - a failing save with save_first=true returns an error and the session
//     stays open (registry entry + tool data intact) so the caller can retry;
//   - the ClaireonSafeExec crash flag surfaces as an explicit "save skipped"
//     error instead of a silent skip, and the session stays open;
//   - save_first=false still closes the session (existing behavior);
//   - 'save' is honored as a legacy alias for save_first (older tool
//     descriptions instructed callers to pass save=true), and save_first
//     takes precedence when both fields are present.
//
// Save failure is injected deterministically by saving the fixture package to
// disk and then marking the .uasset read-only (source control is not involved
// in the Untest environment, so the engine save path fails with PR_Failure).
// All fixture assets live under /Game/Tests/ClaireonCloseSave and are deleted
// (object + disk file) in each test's teardown.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonAnimTools_Session.h"
#include "Tools/ClaireonBehaviorTreeTool_Open.h"
#include "Tools/ClaireonBehaviorTreeTool_Close.h"
#include "Tools/ClaireonBlackboardTool_Open.h"
#include "Tools/ClaireonBlackboardTool_Close.h"
#include "ClaireonSafeExec.h"
#include "ClaireonSessionManager.h"

#include "Animation/AnimComposite.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTreeGraph.h"
#include "EdGraphSchema_BehaviorTree.h"
#include "Kismet2/BlueprintEditorUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonCloseSaveContractTestsInternal
{

static const TCHAR* WI9CloseSave_FixtureFolder = TEXT("/Game/Tests/ClaireonCloseSave");

// --- generic helpers --------------------------------------------------------

FString WI9CloseSave_PackageFileName(const FString& PackagePath)
{
	return FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
}

// Best-effort removal of leftovers from a previous (possibly failed) run:
// clears read-only, force-deletes the in-memory asset, deletes the disk file.
void WI9CloseSave_PreClean(const FString& PackagePath)
{
	const FString FileName = WI9CloseSave_PackageFileName(PackagePath);
	if (IFileManager::Get().FileExists(*FileName))
	{
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*FileName, false);
	}

	const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetShortName(PackagePath);
	if (UObject* Existing = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Existing))
	{
		TArray<UObject*> ToDelete;
		ToDelete.Add(Existing);
		ClaireonTestAssetDeletion::DeleteObjectsForTest(ToDelete);
	}

	if (IFileManager::Get().FileExists(*FileName))
	{
		IFileManager::Get().Delete(*FileName);
	}
}

// Teardown: force-delete the asset object and remove the disk file (clearing
// read-only first so a mid-test failure cannot leave an undeletable file).
void WI9CloseSave_DeleteFixture(UObject* Asset, const FString& PackagePath)
{
	if (IsValid(Asset))
	{
		TArray<UObject*> ToDelete;
		ToDelete.Add(Asset);
		ClaireonTestAssetDeletion::DeleteObjectsForTest(ToDelete);
	}

	const FString FileName = WI9CloseSave_PackageFileName(PackagePath);
	if (IFileManager::Get().FileExists(*FileName))
	{
		FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*FileName, false);
		IFileManager::Get().Delete(*FileName);
	}
}

bool WI9CloseSave_SavePackageToDisk(UObject* Asset, const FString& PackagePath)
{
	if (!IsValid(Asset))
	{
		return false;
	}
	const FString FileName = WI9CloseSave_PackageFileName(PackagePath);
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	const FSavePackageResultStruct Result = UPackage::Save(Asset->GetPackage(), Asset, *FileName, SaveArgs);
	return Result.IsSuccessful() && IFileManager::Get().FileExists(*FileName);
}

bool WI9CloseSave_SetFileReadOnly(const FString& PackagePath, bool bReadOnly)
{
	const FString FileName = WI9CloseSave_PackageFileName(PackagePath);
	return FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*FileName, bReadOnly);
}

FString WI9CloseSave_SessionIdFrom(const IClaireonTool::FToolResult& Result)
{
	FString SessionId;
	if (!Result.bIsError && Result.Data.IsValid())
	{
		Result.Data->TryGetStringField(TEXT("session_id"), SessionId);
	}
	return SessionId;
}

bool WI9CloseSave_SessionExists(const FString& SessionId)
{
	return FClaireonSessionManager::Get().FindSession(SessionId) != nullptr;
}

TSharedPtr<FJsonObject> WI9CloseSave_OpenArgs(const FString& AssetPath)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	return Args;
}

TSharedPtr<FJsonObject> WI9CloseSave_CloseArgs(const FString& SessionId, bool bSaveFirst)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetBoolField(TEXT("save_first"), bSaveFirst);
	return Args;
}

// Legacy alias form: only the 'save' field, no 'save_first'.
TSharedPtr<FJsonObject> WI9CloseSave_CloseArgsSaveAlias(const FString& SessionId, bool bSaveAlias)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetBoolField(TEXT("save"), bSaveAlias);
	return Args;
}

// Both fields present: 'save_first' must win over the 'save' alias.
TSharedPtr<FJsonObject> WI9CloseSave_CloseArgsBoth(const FString& SessionId, bool bSaveFirst, bool bSaveAlias)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetBoolField(TEXT("save_first"), bSaveFirst);
	Args->SetBoolField(TEXT("save"), bSaveAlias);
	return Args;
}

// Expected error texts -- must match the tool implementations exactly.

FString WI9CloseSave_ExpectedSaveFailError(const FString& AssetObjectPath, const FString& SessionId)
{
	return FString::Printf(
		TEXT("save_first failed: %s did not save (the package save was rejected; check for a read-only file or source-control lock). The asset was NOT saved and session %s is still open. Fix the save blocker and retry, or close with save_first=false to discard in-session edits."),
		*AssetObjectPath, *SessionId);
}

FString WI9CloseSave_ExpectedCrashFlagError(const FString& AssetObjectPath, const FString& SessionId)
{
	return FString::Printf(
		TEXT("save_first failed: save of %s was skipped because a previous tool execution crashed (ClaireonSafeExec crash flag is set) and editor state may be corrupted. The asset was NOT saved and session %s is still open. Verify editor state (restart if needed) and retry, or close with save_first=false to discard in-session edits."),
		*AssetObjectPath, *SessionId);
}

// --- fixture creation -------------------------------------------------------

// Creates an in-memory AnimComposite fixture (simplest concrete
// UAnimSequenceBase: no skeleton or compressed data required).
UAnimComposite* WI9CloseSave_CreateAnimComposite(const FString& PackagePath)
{
	UPackage* Package = CreatePackage(*PackagePath);
	if (!IsValid(Package))
	{
		return nullptr;
	}
	const FString AssetName = FPackageName::GetShortName(PackagePath);
	UAnimComposite* Composite = NewObject<UAnimComposite>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (IsValid(Composite))
	{
		FAssetRegistryModule::AssetCreated(Composite);
	}
	return Composite;
}

// Creates an in-memory Behavior Tree fixture with an editor BTGraph (required
// by behaviortree_open, which rejects assets without one).
UBehaviorTree* WI9CloseSave_CreateBehaviorTree(const FString& PackagePath)
{
	UPackage* Package = CreatePackage(*PackagePath);
	if (!IsValid(Package))
	{
		return nullptr;
	}
	const FString AssetName = FPackageName::GetShortName(PackagePath);
	UBehaviorTree* BT = NewObject<UBehaviorTree>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (!IsValid(BT))
	{
		return nullptr;
	}

	UEdGraph* Graph = FBlueprintEditorUtils::CreateNewGraph(
		BT, TEXT("Behavior Tree"), UBehaviorTreeGraph::StaticClass(), UEdGraphSchema_BehaviorTree::StaticClass());
	UBehaviorTreeGraph* BTGraph = Cast<UBehaviorTreeGraph>(Graph);
	if (!IsValid(BTGraph))
	{
		return nullptr;
	}
	BT->BTGraph = BTGraph;
	if (const UEdGraphSchema* Schema = Graph->GetSchema(); IsValid(Schema))
	{
		Schema->CreateDefaultNodesForGraph(*Graph);
	}
	// Mirror FBehaviorTreeEditor::RestoreBehaviorTree's fresh-graph init.
	BTGraph->OnCreated();
	BTGraph->Initialize();

	FAssetRegistryModule::AssetCreated(BT);
	return BT;
}

// Creates an in-memory Blackboard fixture.
UBlackboardData* WI9CloseSave_CreateBlackboard(const FString& PackagePath)
{
	UPackage* Package = CreatePackage(*PackagePath);
	if (!IsValid(Package))
	{
		return nullptr;
	}
	const FString AssetName = FPackageName::GetShortName(PackagePath);
	UBlackboardData* BB = NewObject<UBlackboardData>(Package, FName(*AssetName), RF_Public | RF_Standalone | RF_Transactional);
	if (IsValid(BB))
	{
		FAssetRegistryModule::AssetCreated(BB);
	}
	return BB;
}

} // namespace ClaireonCloseSaveContractTestsInternal

using namespace ClaireonCloseSaveContractTestsInternal;

// ============================================================================
// anim_close
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, CloseSaveContract, AnimClose_SaveFirstFalse_ClosesSession, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/AC_WI9_NoSave"), WI9CloseSave_FixtureFolder);
	WI9CloseSave_PreClean(PackagePath);

	UAnimComposite* Composite = WI9CloseSave_CreateAnimComposite(PackagePath);
	UNTEST_ASSERT_PTR(Composite);

	ClaireonAnimTool_Open OpenTool;
	ClaireonAnimTool_Close CloseTool;

	auto OpenResult = OpenTool.Execute(WI9CloseSave_OpenArgs(PackagePath));
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	const FString SessionId = WI9CloseSave_SessionIdFrom(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	auto CloseResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, false));
	UNTEST_EXPECT_FALSE(CloseResult.bIsError);
	UNTEST_EXPECT_FALSE(WI9CloseSave_SessionExists(SessionId));
	UNTEST_EXPECT_FALSE(ClaireonAnimEditToolBase::ToolData.Contains(SessionId));

	WI9CloseSave_DeleteFixture(Composite, PackagePath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CloseSaveContract, AnimClose_CrashFlag_ErrorsAndKeepsSessionOpen, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/AC_WI9_CrashFlag"), WI9CloseSave_FixtureFolder);
	WI9CloseSave_PreClean(PackagePath);

	UAnimComposite* Composite = WI9CloseSave_CreateAnimComposite(PackagePath);
	UNTEST_ASSERT_PTR(Composite);

	ClaireonAnimTool_Open OpenTool;
	ClaireonAnimTool_Close CloseTool;

	auto OpenResult = OpenTool.Execute(WI9CloseSave_OpenArgs(PackagePath));
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	const FString SessionId = WI9CloseSave_SessionIdFrom(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	// Simulate crash-recovery state, then attempt close with save_first=true.
	// Clear the flag immediately after Execute so a failed assertion below
	// cannot leak crash state into other tests.
	ClaireonSafeExec::SetCrashFlag();
	auto CloseResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, true));
	const bool bSessionStillOpen = WI9CloseSave_SessionExists(SessionId);
	ClaireonSafeExec::ClearCrashFlag();

	UNTEST_ASSERT_TRUE(CloseResult.bIsError);
	const FString ExpectedError = WI9CloseSave_ExpectedCrashFlagError(Composite->GetPathName(), SessionId);
	UNTEST_EXPECT_STREQ(*CloseResult.ErrorMessage, *ExpectedError);
	UNTEST_EXPECT_TRUE(bSessionStillOpen);
	UNTEST_EXPECT_TRUE(ClaireonAnimEditToolBase::ToolData.Contains(SessionId));

	// With the flag cleared, the session is still usable: close without save.
	auto RetryResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, false));
	UNTEST_EXPECT_FALSE(RetryResult.bIsError);
	UNTEST_EXPECT_FALSE(WI9CloseSave_SessionExists(SessionId));

	WI9CloseSave_DeleteFixture(Composite, PackagePath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CloseSaveContract, AnimClose_FailingSave_ErrorsAndKeepsSessionOpen, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/AC_WI9_SaveFail"), WI9CloseSave_FixtureFolder);
	WI9CloseSave_PreClean(PackagePath);

	UAnimComposite* Composite = WI9CloseSave_CreateAnimComposite(PackagePath);
	UNTEST_ASSERT_PTR(Composite);
	UNTEST_ASSERT_TRUE(WI9CloseSave_SavePackageToDisk(Composite, PackagePath));

	ClaireonAnimTool_Open OpenTool;
	ClaireonAnimTool_Close CloseTool;

	auto OpenResult = OpenTool.Execute(WI9CloseSave_OpenArgs(PackagePath));
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	const FString SessionId = WI9CloseSave_SessionIdFrom(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	// Make the on-disk .uasset read-only so the save deterministically fails.
	// Capture outcome and restore writability BEFORE asserting so a failed
	// assertion cannot leave a read-only file under Content.
	UNTEST_ASSERT_TRUE(WI9CloseSave_SetFileReadOnly(PackagePath, true));
	auto CloseResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, true));
	const bool bSessionStillOpen = WI9CloseSave_SessionExists(SessionId);
	WI9CloseSave_SetFileReadOnly(PackagePath, false);

	UNTEST_ASSERT_TRUE(CloseResult.bIsError);
	const FString ExpectedError = WI9CloseSave_ExpectedSaveFailError(Composite->GetPathName(), SessionId);
	UNTEST_EXPECT_STREQ(*CloseResult.ErrorMessage, *ExpectedError);
	UNTEST_EXPECT_TRUE(bSessionStillOpen);
	UNTEST_EXPECT_TRUE(ClaireonAnimEditToolBase::ToolData.Contains(SessionId));

	// After fixing the blocker, the same save_first=true close must succeed.
	auto RetryResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, true));
	UNTEST_EXPECT_FALSE(RetryResult.bIsError);
	UNTEST_EXPECT_FALSE(WI9CloseSave_SessionExists(SessionId));

	WI9CloseSave_DeleteFixture(Composite, PackagePath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CloseSaveContract, AnimClose_SaveAlias_TriggersSaveAndSaveFirstWins, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/AC_WI9_SaveAlias"), WI9CloseSave_FixtureFolder);
	WI9CloseSave_PreClean(PackagePath);

	UAnimComposite* Composite = WI9CloseSave_CreateAnimComposite(PackagePath);
	UNTEST_ASSERT_PTR(Composite);
	UNTEST_ASSERT_TRUE(WI9CloseSave_SavePackageToDisk(Composite, PackagePath));

	ClaireonAnimTool_Open OpenTool;
	ClaireonAnimTool_Close CloseTool;

	auto OpenResult = OpenTool.Execute(WI9CloseSave_OpenArgs(PackagePath));
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	const FString SessionId = WI9CloseSave_SessionIdFrom(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	// With the file read-only, close with ONLY the legacy 'save' alias: the
	// alias must trigger the save path, so the close errors and the session
	// stays open. Then close with save_first=false AND save=true: the schema
	// field wins, no save is attempted, and the close succeeds even though the
	// file is still read-only. Writability is restored before asserting.
	UNTEST_ASSERT_TRUE(WI9CloseSave_SetFileReadOnly(PackagePath, true));
	auto AliasResult = CloseTool.Execute(WI9CloseSave_CloseArgsSaveAlias(SessionId, true));
	const bool bSessionOpenAfterAlias = WI9CloseSave_SessionExists(SessionId);
	const bool bToolDataAfterAlias = ClaireonAnimEditToolBase::ToolData.Contains(SessionId);
	auto PrecedenceResult = CloseTool.Execute(WI9CloseSave_CloseArgsBoth(SessionId, false, true));
	const bool bSessionOpenAfterPrecedence = WI9CloseSave_SessionExists(SessionId);
	WI9CloseSave_SetFileReadOnly(PackagePath, false);

	UNTEST_ASSERT_TRUE(AliasResult.bIsError);
	const FString ExpectedError = WI9CloseSave_ExpectedSaveFailError(Composite->GetPathName(), SessionId);
	UNTEST_EXPECT_STREQ(*AliasResult.ErrorMessage, *ExpectedError);
	UNTEST_EXPECT_TRUE(bSessionOpenAfterAlias);
	UNTEST_EXPECT_TRUE(bToolDataAfterAlias);
	UNTEST_EXPECT_FALSE(PrecedenceResult.bIsError);
	UNTEST_EXPECT_FALSE(bSessionOpenAfterPrecedence);

	WI9CloseSave_DeleteFixture(Composite, PackagePath);
	co_return;
}

// ============================================================================
// behaviortree_close
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, CloseSaveContract, BTClose_SaveFirstFalse_ClosesSession, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BT_WI9_NoSave"), WI9CloseSave_FixtureFolder);
	WI9CloseSave_PreClean(PackagePath);

	UBehaviorTree* BT = WI9CloseSave_CreateBehaviorTree(PackagePath);
	UNTEST_ASSERT_PTR(BT);

	ClaireonBehaviorTreeTool_Open OpenTool;
	ClaireonBehaviorTreeTool_Close CloseTool;

	auto OpenResult = OpenTool.Execute(WI9CloseSave_OpenArgs(PackagePath));
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	const FString SessionId = WI9CloseSave_SessionIdFrom(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	auto CloseResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, false));
	UNTEST_EXPECT_FALSE(CloseResult.bIsError);
	UNTEST_EXPECT_FALSE(WI9CloseSave_SessionExists(SessionId));
	UNTEST_EXPECT_FALSE(ClaireonBehaviorTreeEditToolBase::ToolData.Contains(SessionId));

	WI9CloseSave_DeleteFixture(BT, PackagePath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CloseSaveContract, BTClose_CrashFlag_ErrorsAndKeepsSessionOpen, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BT_WI9_CrashFlag"), WI9CloseSave_FixtureFolder);
	WI9CloseSave_PreClean(PackagePath);

	UBehaviorTree* BT = WI9CloseSave_CreateBehaviorTree(PackagePath);
	UNTEST_ASSERT_PTR(BT);

	ClaireonBehaviorTreeTool_Open OpenTool;
	ClaireonBehaviorTreeTool_Close CloseTool;

	auto OpenResult = OpenTool.Execute(WI9CloseSave_OpenArgs(PackagePath));
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	const FString SessionId = WI9CloseSave_SessionIdFrom(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	ClaireonSafeExec::SetCrashFlag();
	auto CloseResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, true));
	const bool bSessionStillOpen = WI9CloseSave_SessionExists(SessionId);
	ClaireonSafeExec::ClearCrashFlag();

	UNTEST_ASSERT_TRUE(CloseResult.bIsError);
	const FString ExpectedError = WI9CloseSave_ExpectedCrashFlagError(BT->GetPathName(), SessionId);
	UNTEST_EXPECT_STREQ(*CloseResult.ErrorMessage, *ExpectedError);
	UNTEST_EXPECT_TRUE(bSessionStillOpen);
	UNTEST_EXPECT_TRUE(ClaireonBehaviorTreeEditToolBase::ToolData.Contains(SessionId));

	auto RetryResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, false));
	UNTEST_EXPECT_FALSE(RetryResult.bIsError);
	UNTEST_EXPECT_FALSE(WI9CloseSave_SessionExists(SessionId));

	WI9CloseSave_DeleteFixture(BT, PackagePath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CloseSaveContract, BTClose_FailingSave_ErrorsAndKeepsSessionOpen, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BT_WI9_SaveFail"), WI9CloseSave_FixtureFolder);
	WI9CloseSave_PreClean(PackagePath);

	UBehaviorTree* BT = WI9CloseSave_CreateBehaviorTree(PackagePath);
	UNTEST_ASSERT_PTR(BT);
	UNTEST_ASSERT_TRUE(WI9CloseSave_SavePackageToDisk(BT, PackagePath));

	ClaireonBehaviorTreeTool_Open OpenTool;
	ClaireonBehaviorTreeTool_Close CloseTool;

	auto OpenResult = OpenTool.Execute(WI9CloseSave_OpenArgs(PackagePath));
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	const FString SessionId = WI9CloseSave_SessionIdFrom(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	UNTEST_ASSERT_TRUE(WI9CloseSave_SetFileReadOnly(PackagePath, true));
	auto CloseResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, true));
	const bool bSessionStillOpen = WI9CloseSave_SessionExists(SessionId);
	WI9CloseSave_SetFileReadOnly(PackagePath, false);

	UNTEST_ASSERT_TRUE(CloseResult.bIsError);
	const FString ExpectedError = WI9CloseSave_ExpectedSaveFailError(BT->GetPathName(), SessionId);
	UNTEST_EXPECT_STREQ(*CloseResult.ErrorMessage, *ExpectedError);
	UNTEST_EXPECT_TRUE(bSessionStillOpen);
	UNTEST_EXPECT_TRUE(ClaireonBehaviorTreeEditToolBase::ToolData.Contains(SessionId));

	auto RetryResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, true));
	UNTEST_EXPECT_FALSE(RetryResult.bIsError);
	UNTEST_EXPECT_FALSE(WI9CloseSave_SessionExists(SessionId));

	WI9CloseSave_DeleteFixture(BT, PackagePath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CloseSaveContract, BTClose_SaveAlias_TriggersSaveAndSaveFirstWins, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BT_WI9_SaveAlias"), WI9CloseSave_FixtureFolder);
	WI9CloseSave_PreClean(PackagePath);

	UBehaviorTree* BT = WI9CloseSave_CreateBehaviorTree(PackagePath);
	UNTEST_ASSERT_PTR(BT);
	UNTEST_ASSERT_TRUE(WI9CloseSave_SavePackageToDisk(BT, PackagePath));

	ClaireonBehaviorTreeTool_Open OpenTool;
	ClaireonBehaviorTreeTool_Close CloseTool;

	auto OpenResult = OpenTool.Execute(WI9CloseSave_OpenArgs(PackagePath));
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	const FString SessionId = WI9CloseSave_SessionIdFrom(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	// Alias-only close must trigger the save path (error, session open);
	// save_first=false + save=true must skip the save and close (schema field
	// wins). See the anim variant for the full rationale.
	UNTEST_ASSERT_TRUE(WI9CloseSave_SetFileReadOnly(PackagePath, true));
	auto AliasResult = CloseTool.Execute(WI9CloseSave_CloseArgsSaveAlias(SessionId, true));
	const bool bSessionOpenAfterAlias = WI9CloseSave_SessionExists(SessionId);
	const bool bToolDataAfterAlias = ClaireonBehaviorTreeEditToolBase::ToolData.Contains(SessionId);
	auto PrecedenceResult = CloseTool.Execute(WI9CloseSave_CloseArgsBoth(SessionId, false, true));
	const bool bSessionOpenAfterPrecedence = WI9CloseSave_SessionExists(SessionId);
	WI9CloseSave_SetFileReadOnly(PackagePath, false);

	UNTEST_ASSERT_TRUE(AliasResult.bIsError);
	const FString ExpectedError = WI9CloseSave_ExpectedSaveFailError(BT->GetPathName(), SessionId);
	UNTEST_EXPECT_STREQ(*AliasResult.ErrorMessage, *ExpectedError);
	UNTEST_EXPECT_TRUE(bSessionOpenAfterAlias);
	UNTEST_EXPECT_TRUE(bToolDataAfterAlias);
	UNTEST_EXPECT_FALSE(PrecedenceResult.bIsError);
	UNTEST_EXPECT_FALSE(bSessionOpenAfterPrecedence);

	WI9CloseSave_DeleteFixture(BT, PackagePath);
	co_return;
}

// ============================================================================
// blackboard_close
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, CloseSaveContract, BBClose_SaveFirstFalse_ClosesSession, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BB_WI9_NoSave"), WI9CloseSave_FixtureFolder);
	WI9CloseSave_PreClean(PackagePath);

	UBlackboardData* BB = WI9CloseSave_CreateBlackboard(PackagePath);
	UNTEST_ASSERT_PTR(BB);

	ClaireonBlackboardTool_Open OpenTool;
	ClaireonBlackboardTool_Close CloseTool;

	auto OpenResult = OpenTool.Execute(WI9CloseSave_OpenArgs(PackagePath));
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	const FString SessionId = WI9CloseSave_SessionIdFrom(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	auto CloseResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, false));
	UNTEST_EXPECT_FALSE(CloseResult.bIsError);
	UNTEST_EXPECT_FALSE(WI9CloseSave_SessionExists(SessionId));
	UNTEST_EXPECT_FALSE(ClaireonBlackboardEditToolBase::ToolData.Contains(SessionId));

	WI9CloseSave_DeleteFixture(BB, PackagePath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CloseSaveContract, BBClose_CrashFlag_ErrorsAndKeepsSessionOpen, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BB_WI9_CrashFlag"), WI9CloseSave_FixtureFolder);
	WI9CloseSave_PreClean(PackagePath);

	UBlackboardData* BB = WI9CloseSave_CreateBlackboard(PackagePath);
	UNTEST_ASSERT_PTR(BB);

	ClaireonBlackboardTool_Open OpenTool;
	ClaireonBlackboardTool_Close CloseTool;

	auto OpenResult = OpenTool.Execute(WI9CloseSave_OpenArgs(PackagePath));
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	const FString SessionId = WI9CloseSave_SessionIdFrom(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	ClaireonSafeExec::SetCrashFlag();
	auto CloseResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, true));
	const bool bSessionStillOpen = WI9CloseSave_SessionExists(SessionId);
	ClaireonSafeExec::ClearCrashFlag();

	UNTEST_ASSERT_TRUE(CloseResult.bIsError);
	const FString ExpectedError = WI9CloseSave_ExpectedCrashFlagError(BB->GetPathName(), SessionId);
	UNTEST_EXPECT_STREQ(*CloseResult.ErrorMessage, *ExpectedError);
	UNTEST_EXPECT_TRUE(bSessionStillOpen);
	UNTEST_EXPECT_TRUE(ClaireonBlackboardEditToolBase::ToolData.Contains(SessionId));

	auto RetryResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, false));
	UNTEST_EXPECT_FALSE(RetryResult.bIsError);
	UNTEST_EXPECT_FALSE(WI9CloseSave_SessionExists(SessionId));

	WI9CloseSave_DeleteFixture(BB, PackagePath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CloseSaveContract, BBClose_FailingSave_ErrorsAndKeepsSessionOpen, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BB_WI9_SaveFail"), WI9CloseSave_FixtureFolder);
	WI9CloseSave_PreClean(PackagePath);

	UBlackboardData* BB = WI9CloseSave_CreateBlackboard(PackagePath);
	UNTEST_ASSERT_PTR(BB);
	UNTEST_ASSERT_TRUE(WI9CloseSave_SavePackageToDisk(BB, PackagePath));

	ClaireonBlackboardTool_Open OpenTool;
	ClaireonBlackboardTool_Close CloseTool;

	auto OpenResult = OpenTool.Execute(WI9CloseSave_OpenArgs(PackagePath));
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	const FString SessionId = WI9CloseSave_SessionIdFrom(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	UNTEST_ASSERT_TRUE(WI9CloseSave_SetFileReadOnly(PackagePath, true));
	auto CloseResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, true));
	const bool bSessionStillOpen = WI9CloseSave_SessionExists(SessionId);
	WI9CloseSave_SetFileReadOnly(PackagePath, false);

	UNTEST_ASSERT_TRUE(CloseResult.bIsError);
	const FString ExpectedError = WI9CloseSave_ExpectedSaveFailError(BB->GetPathName(), SessionId);
	UNTEST_EXPECT_STREQ(*CloseResult.ErrorMessage, *ExpectedError);
	UNTEST_EXPECT_TRUE(bSessionStillOpen);
	UNTEST_EXPECT_TRUE(ClaireonBlackboardEditToolBase::ToolData.Contains(SessionId));

	auto RetryResult = CloseTool.Execute(WI9CloseSave_CloseArgs(SessionId, true));
	UNTEST_EXPECT_FALSE(RetryResult.bIsError);
	UNTEST_EXPECT_FALSE(WI9CloseSave_SessionExists(SessionId));

	WI9CloseSave_DeleteFixture(BB, PackagePath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, CloseSaveContract, BBClose_SaveAlias_TriggersSaveAndSaveFirstWins, UNTEST_TIMEOUTMS(60000))
{
	const FString PackagePath = FString::Printf(TEXT("%s/BB_WI9_SaveAlias"), WI9CloseSave_FixtureFolder);
	WI9CloseSave_PreClean(PackagePath);

	UBlackboardData* BB = WI9CloseSave_CreateBlackboard(PackagePath);
	UNTEST_ASSERT_PTR(BB);
	UNTEST_ASSERT_TRUE(WI9CloseSave_SavePackageToDisk(BB, PackagePath));

	ClaireonBlackboardTool_Open OpenTool;
	ClaireonBlackboardTool_Close CloseTool;

	auto OpenResult = OpenTool.Execute(WI9CloseSave_OpenArgs(PackagePath));
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	const FString SessionId = WI9CloseSave_SessionIdFrom(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	// Alias-only close must trigger the save path (error, session open);
	// save_first=false + save=true must skip the save and close (schema field
	// wins). See the anim variant for the full rationale.
	UNTEST_ASSERT_TRUE(WI9CloseSave_SetFileReadOnly(PackagePath, true));
	auto AliasResult = CloseTool.Execute(WI9CloseSave_CloseArgsSaveAlias(SessionId, true));
	const bool bSessionOpenAfterAlias = WI9CloseSave_SessionExists(SessionId);
	const bool bToolDataAfterAlias = ClaireonBlackboardEditToolBase::ToolData.Contains(SessionId);
	auto PrecedenceResult = CloseTool.Execute(WI9CloseSave_CloseArgsBoth(SessionId, false, true));
	const bool bSessionOpenAfterPrecedence = WI9CloseSave_SessionExists(SessionId);
	WI9CloseSave_SetFileReadOnly(PackagePath, false);

	UNTEST_ASSERT_TRUE(AliasResult.bIsError);
	const FString ExpectedError = WI9CloseSave_ExpectedSaveFailError(BB->GetPathName(), SessionId);
	UNTEST_EXPECT_STREQ(*AliasResult.ErrorMessage, *ExpectedError);
	UNTEST_EXPECT_TRUE(bSessionOpenAfterAlias);
	UNTEST_EXPECT_TRUE(bToolDataAfterAlias);
	UNTEST_EXPECT_FALSE(PrecedenceResult.bIsError);
	UNTEST_EXPECT_FALSE(bSessionOpenAfterPrecedence);

	WI9CloseSave_DeleteFixture(BB, PackagePath);
	co_return;
}

#endif // WITH_UNTESTED
