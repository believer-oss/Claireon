// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_AddRig.h"
#include "Tools/ClaireonCameraAssetTool_Create.h"
#include "Tools/ClaireonCameraAssetTool_Duplicate.h"
#include "Tools/ClaireonCameraAssetTool_Save.h"

#include "Core/CameraAsset.h"
#include "Core/CameraRigAsset.h"
#include "EditorAssetLibrary.h"
#include "Misc/AutomationTest.h"
#include "PackageTools.h"
#include "UObject/Package.h"

#include "Dom/JsonObject.h"

namespace
{
	/** Best-effort cleanup of a /Game/Tests/<X> asset; ignores absence. */
	void CALifecycleSpec_DeleteIfExists(const FString& Path)
	{
		if (UEditorAssetLibrary::DoesAssetExist(Path))
		{
			UEditorAssetLibrary::DeleteAsset(Path);
		}
	}

	TSharedPtr<FJsonObject> CALifecycleSpec_StringArg(const FString& Key, const FString& Value)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(Key, Value);
		return Args;
	}

	TSharedPtr<FJsonObject> CALifecycleSpec_TwoStringArgs(
		const FString& K1, const FString& V1,
		const FString& K2, const FString& V2)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(K1, V1);
		Args->SetStringField(K2, V2);
		return Args;
	}

	IClaireonTool::FToolResult CALifecycleSpec_Create(const FString& Path)
	{
		FClaireonCameraAssetTool_Create Tool;
		return Tool.Execute(CALifecycleSpec_StringArg(TEXT("asset_path"), Path));
	}

	IClaireonTool::FToolResult CALifecycleSpec_Duplicate(const FString& Src, const FString& Dest)
	{
		FClaireonCameraAssetTool_Duplicate Tool;
		return Tool.Execute(CALifecycleSpec_TwoStringArgs(
			TEXT("source_path"), Src,
			TEXT("dest_path"), Dest));
	}
} // namespace

// =====================================================================================
// Test: Create_HappyPath
// Create a UCameraAsset at a /Game/Tests/* path; verify it loads and has 0 rigs.
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetLifecycle_Create_HappyPath,
	"Claireon.CameraAsset.Lifecycle.Create_HappyPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetLifecycle_Create_HappyPath::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/CA_Lifecycle_Empty");
	CALifecycleSpec_DeleteIfExists(Path);

	const auto Result = CALifecycleSpec_Create(Path);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Create returned error: %s"), *Result.ErrorMessage));
		return false;
	}

	UCameraAsset* Asset = LoadObject<UCameraAsset>(nullptr, *Path);
	if (!Asset)
	{
		AddError(TEXT("Asset not found after Create"));
		return false;
	}
	const int32 RigCount = Asset->GetCameraRigs().Num();
	if (RigCount != 0)
	{
		AddError(FString::Printf(TEXT("Expected 0 rigs after Create; got %d"), RigCount));
		return false;
	}

	CALifecycleSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: Create_PathOutsideGame
// asset_path that does not start with /Game/ must return an error.
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetLifecycle_Create_PathOutsideGame,
	"Claireon.CameraAsset.Lifecycle.Create_PathOutsideGame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetLifecycle_Create_PathOutsideGame::RunTest(const FString& /*Parameters*/)
{
	const auto Result = CALifecycleSpec_Create(TEXT("/NotGame/Foo"));
	if (!Result.bIsError)
	{
		AddError(TEXT("Create accepted a non-/Game/ path"));
		return false;
	}
	return true;
}

// =====================================================================================
// Test: Create_AlreadyExists
// Calling Create twice on the same path must error on the second call; first is preserved.
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetLifecycle_Create_AlreadyExists,
	"Claireon.CameraAsset.Lifecycle.Create_AlreadyExists",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetLifecycle_Create_AlreadyExists::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/CA_Lifecycle_Twice");
	CALifecycleSpec_DeleteIfExists(Path);

	const auto First = CALifecycleSpec_Create(Path);
	if (First.bIsError)
	{
		AddError(FString::Printf(TEXT("First Create errored unexpectedly: %s"), *First.ErrorMessage));
		return false;
	}

	const auto Second = CALifecycleSpec_Create(Path);
	if (!Second.bIsError)
	{
		AddError(TEXT("Second Create on the same path did not error"));
		CALifecycleSpec_DeleteIfExists(Path);
		return false;
	}

	UCameraAsset* Asset = LoadObject<UCameraAsset>(nullptr, *Path);
	if (!Asset)
	{
		AddError(TEXT("Asset disappeared after second Create"));
		return false;
	}

	CALifecycleSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: Duplicate_HappyPath
// Create A, Duplicate to B; B exists and is loadable as UCameraAsset.
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetLifecycle_Duplicate_HappyPath,
	"Claireon.CameraAsset.Lifecycle.Duplicate_HappyPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetLifecycle_Duplicate_HappyPath::RunTest(const FString& /*Parameters*/)
{
	const FString Src = TEXT("/Game/Tests/CA_Lifecycle_DupSrc");
	const FString Dst = TEXT("/Game/Tests/CA_Lifecycle_DupDst");
	CALifecycleSpec_DeleteIfExists(Src);
	CALifecycleSpec_DeleteIfExists(Dst);

	const auto CreateResult = CALifecycleSpec_Create(Src);
	if (CreateResult.bIsError)
	{
		AddError(FString::Printf(TEXT("Create failed: %s"), *CreateResult.ErrorMessage));
		return false;
	}

	const auto DupResult = CALifecycleSpec_Duplicate(Src, Dst);
	if (DupResult.bIsError)
	{
		AddError(FString::Printf(TEXT("Duplicate errored: %s"), *DupResult.ErrorMessage));
		CALifecycleSpec_DeleteIfExists(Src);
		return false;
	}

	UCameraAsset* DupAsset = LoadObject<UCameraAsset>(nullptr, *Dst);
	if (!DupAsset)
	{
		AddError(TEXT("Duplicate dest not loadable as UCameraAsset"));
		CALifecycleSpec_DeleteIfExists(Src);
		return false;
	}

	CALifecycleSpec_DeleteIfExists(Src);
	CALifecycleSpec_DeleteIfExists(Dst);
	return true;
}

// =====================================================================================
// Test: Duplicate_MissingSource
// Duplicating from a nonexistent path must error.
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetLifecycle_Duplicate_MissingSource,
	"Claireon.CameraAsset.Lifecycle.Duplicate_MissingSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetLifecycle_Duplicate_MissingSource::RunTest(const FString& /*Parameters*/)
{
	const auto Result = CALifecycleSpec_Duplicate(
		TEXT("/Game/Tests/CA_Lifecycle_NeverExisted"),
		TEXT("/Game/Tests/CA_Lifecycle_DupShouldFail"));
	if (!Result.bIsError)
	{
		AddError(TEXT("Duplicate accepted a missing source"));
		CALifecycleSpec_DeleteIfExists(TEXT("/Game/Tests/CA_Lifecycle_DupShouldFail"));
		return false;
	}
	return true;
}

// =====================================================================================
// Test: Save_RoundTrip
// Create asset -> AddRig -> camera_asset_save -> unload package -> reload from disk ->
// verify the rig persisted. Exercises the BuildCamera-pre-run + SavePackages contract.
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCameraAssetLifecycle_Save_RoundTrip,
	"Claireon.CameraAsset.Lifecycle.Save_RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCameraAssetLifecycle_Save_RoundTrip::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/CA_Lifecycle_SaveRT");
	CALifecycleSpec_DeleteIfExists(Path);

	// 1. Create asset.
	{
		const auto CreateResult = CALifecycleSpec_Create(Path);
		if (CreateResult.bIsError)
		{
			AddError(FString::Printf(TEXT("Create failed: %s"), *CreateResult.ErrorMessage));
			return false;
		}
	}

	// 2. AddRig "Rig0".
	{
		FClaireonCameraAssetTool_AddRig Tool;
		const auto Result = Tool.Execute(CALifecycleSpec_TwoStringArgs(
			TEXT("asset_path"), Path,
			TEXT("rig_name"), TEXT("Rig0")));
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("AddRig failed: %s"), *Result.ErrorMessage));
			CALifecycleSpec_DeleteIfExists(Path);
			return false;
		}
	}

	// 3. camera_asset_save.
	{
		FClaireonCameraAssetTool_Save Tool;
		const auto Result = Tool.Execute(CALifecycleSpec_StringArg(TEXT("asset_path"), Path));
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Save returned error: %s"), *Result.ErrorMessage));
			CALifecycleSpec_DeleteIfExists(Path);
			return false;
		}
		if (!Result.Data.IsValid())
		{
			AddError(TEXT("Save returned no data"));
			CALifecycleSpec_DeleteIfExists(Path);
			return false;
		}
		bool bSuccess = false;
		if (!Result.Data->TryGetBoolField(TEXT("success"), bSuccess) || !bSuccess)
		{
			AddError(TEXT("Save reported success=false on a clean asset"));
			CALifecycleSpec_DeleteIfExists(Path);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* BuildLogPtr = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("build_log"), BuildLogPtr) || !BuildLogPtr)
		{
			AddError(TEXT("Save result missing 'build_log' array"));
			CALifecycleSpec_DeleteIfExists(Path);
			return false;
		}
	}

	// 4. Unload the package so a fresh LoadObject re-pulls from disk.
	if (UPackage* Pkg = FindPackage(nullptr, *Path))
	{
		FText UnloadErr;
		const bool bUnloaded = UPackageTools::UnloadPackages({ Pkg }, UnloadErr, /*bUnloadDirtyPackages=*/true);
		if (!bUnloaded)
		{
			AddError(FString::Printf(TEXT("UnloadPackages failed: %s"), *UnloadErr.ToString()));
			CALifecycleSpec_DeleteIfExists(Path);
			return false;
		}
	}

	// 5. Reload + verify the rig persisted.
	UCameraAsset* Reloaded = LoadObject<UCameraAsset>(nullptr, *Path);
	if (!Reloaded)
	{
		AddError(TEXT("Asset failed to reload after unload"));
		CALifecycleSpec_DeleteIfExists(Path);
		return false;
	}
	const int32 RigCount = Reloaded->GetCameraRigs().Num();
	if (RigCount != 1)
	{
		AddError(FString::Printf(TEXT("Expected 1 rig after save+reload; got %d"), RigCount));
		CALifecycleSpec_DeleteIfExists(Path);
		return false;
	}

	CALifecycleSpec_DeleteIfExists(Path);
	return true;
}
