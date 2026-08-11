// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonDataAssetTool_Create.h"
#include "ClaireonSafeExec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "ClaireonTestAssetDeletion.h"
// ---------------------------------------------------------------------------
// WI-10: data_asset_create must be able to save brand-new assets (no disk file
// yet at SaveAsset time), and a failed save must not leak the in-memory
// registration that made retries report 'Asset already exists'.
// ---------------------------------------------------------------------------

namespace ClaireonDataAssetCreateTestsImpl
{
	// Sandbox folder for transient test content; every asset created here is
	// deleted in teardown.
	static const TCHAR* SandboxRoot = TEXT("/Game/__ClaireonDataAssetCreateTests");

	// Module-local UPrimaryDataAsset fixture (see ClaireonTestTypes.h) so the
	// tests do not depend on any game class.
	static const TCHAR* SpecDataAssetClassPath = TEXT("/Script/Claireon.ClaireonSpecDataAsset");

	/** Build a unique package path under the sandbox to avoid cross-run collisions. */
	static FString MakeUniquePackagePath(const TCHAR* Tag)
	{
		return FString::Printf(TEXT("%s/%s_%s"),
			SandboxRoot, Tag, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	/** Drive the tool exactly as MCP does: Execute with an FJsonObject payload. */
	static IClaireonTool::FToolResult RunCreate(const FString& PackagePath)
	{
		FClaireonDataAssetTool_Create Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), PackagePath);
		Args->SetStringField(TEXT("class_path"), SpecDataAssetClassPath);
		return Tool.Execute(Args);
	}

	/** Best-effort teardown: remove the created asset (registry + disk) and any stray file. */
	static void CleanupPackage(const FString& PackagePath)
	{
		const FString ObjectPath = FString::Printf(TEXT("%s.%s"),
			*PackagePath, *FPackageName::GetShortName(PackagePath));
		if (UEditorAssetLibrary::DoesAssetExist(ObjectPath))
		{
			ClaireonTestAssetDeletion::DeleteAssetForTest(ObjectPath);
		}
		FString FileName;
		if (FPackageName::TryConvertLongPackageNameToFilename(
				PackagePath, FileName, FPackageName::GetAssetPackageExtension())
			&& IFileManager::Get().FileExists(*FileName))
		{
			IFileManager::Get().Delete(*FileName, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		}
	}

	/**
	 * RAII guard that forces ClaireonAssetUtils::SaveAsset to fail
	 * deterministically (its crash-flag pre-check) and guarantees the flag is
	 * cleared even when a test assertion co_returns early.
	 */
	struct FDataAssetCreateTests_ScopedForcedSaveFailure
	{
		FDataAssetCreateTests_ScopedForcedSaveFailure() { ClaireonSafeExec::SetCrashFlag(); }
		~FDataAssetCreateTests_ScopedForcedSaveFailure() { ClaireonSafeExec::ClearCrashFlag(); }
	};
} // namespace ClaireonDataAssetCreateTestsImpl

// ---------------------------------------------------------------------------
// 1. Creating a data asset at a fresh path must succeed AND land on disk.
//    Before the fix, SaveAsset gated on FPackageName::DoesPackageExist, which
//    is false for a freshly created in-memory package, so every brand-new
//    asset failed with 'Package file not found'.
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, DataAssetCreate, FreshPathCreatesAndSavesToDisk, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonDataAssetCreateTestsImpl;
	const FString PackagePath = MakeUniquePackagePath(TEXT("Fresh"));

	IClaireonTool::FToolResult Result = RunCreate(PackagePath);

	// Capture observations before teardown; UNTEST_ASSERT_* co_returns and
	// would otherwise skip cleanup.
	const bool bCreateErrored = Result.bIsError;
	const FString CreateError = Result.ErrorMessage;
	const bool bHasData = Result.Data.IsValid();
	FString ReturnedAssetPath;
	if (bHasData)
	{
		Result.Data->TryGetStringField(TEXT("asset_path"), ReturnedAssetPath);
	}
	FString PackageFileName;
	const bool bOnDisk = FPackageName::DoesPackageExist(PackagePath, &PackageFileName);

	CleanupPackage(PackagePath);

	if (bCreateErrored)
	{
		UNTEST_EXPECT_STREQ(CreateError, TEXT("")); // surface the actual error text in the failure report
	}
	UNTEST_ASSERT_FALSE(bCreateErrored);
	UNTEST_ASSERT_TRUE(bHasData);
	UNTEST_EXPECT_TRUE(ReturnedAssetPath.StartsWith(PackagePath));
	UNTEST_EXPECT_TRUE(bOnDisk);
	UNTEST_EXPECT_FALSE(PackageFileName.IsEmpty());
	co_return;
}

// ---------------------------------------------------------------------------
// 2. A failed save must (a) return an error, (b) leave nothing on disk, and
//    (c) NOT leak the AssetCreated registration: a retry at the same path
//    must not report 'Asset already exists', and once the failure cause is
//    gone the retry must fully succeed.
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, DataAssetCreate, SaveFailureCleansLeakAndRetrySucceeds, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonDataAssetCreateTestsImpl;
	const FString PackagePath = MakeUniquePackagePath(TEXT("SaveFail"));

	bool bFirstErrored = false;
	bool bSecondErrored = false;
	FString FirstError;
	FString SecondError;
	{
		FDataAssetCreateTests_ScopedForcedSaveFailure ForceSaveFailure;

		IClaireonTool::FToolResult First = RunCreate(PackagePath);
		bFirstErrored = First.bIsError;
		FirstError = First.ErrorMessage;

		// Retry while the save still fails: before the fix this reported
		// 'Asset already exists at path: ...' because the failure branch
		// skipped the cleanup its sibling branches perform.
		IClaireonTool::FToolResult Second = RunCreate(PackagePath);
		bSecondErrored = Second.bIsError;
		SecondError = Second.ErrorMessage;
	}

	const bool bOnDiskAfterFailures = FPackageName::DoesPackageExist(PackagePath);

	// Failure cause removed (crash flag cleared by the guard): the same path
	// must now be creatable end-to-end.
	IClaireonTool::FToolResult Third = RunCreate(PackagePath);
	const bool bThirdErrored = Third.bIsError;
	const FString ThirdError = Third.ErrorMessage;
	const bool bOnDiskAfterRetry = FPackageName::DoesPackageExist(PackagePath);

	CleanupPackage(PackagePath);

	UNTEST_ASSERT_TRUE(bFirstErrored);
	// Exact error text contract for the forced-failure seam (SaveAsset's
	// crash-flag pre-check).
	UNTEST_EXPECT_STREQ(FirstError,
		TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));

	UNTEST_ASSERT_TRUE(bSecondErrored);
	UNTEST_EXPECT_FALSE(SecondError.Contains(TEXT("Asset already exists")));
	UNTEST_EXPECT_STREQ(SecondError,
		TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));

	UNTEST_EXPECT_FALSE(bOnDiskAfterFailures);

	if (bThirdErrored)
	{
		UNTEST_EXPECT_STREQ(ThirdError, TEXT("")); // surface the actual error text in the failure report
	}
	UNTEST_ASSERT_FALSE(bThirdErrored);
	UNTEST_EXPECT_TRUE(bOnDiskAfterRetry);
	co_return;
}

#endif // WITH_UNTESTED
