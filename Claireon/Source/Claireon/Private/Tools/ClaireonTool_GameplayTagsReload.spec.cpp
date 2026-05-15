// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Specs for claireon.gameplay_tags_reload (#0000).
// Strategy 1 cleanup: tests use a transient tag source under
// FPaths::ProjectIntermediateDir() / "ClaireonSpecs" so Config/DefaultGameplayTags.ini
// is never touched.

#include "Tools/ClaireonTool_GameplayTagsReload.h"
#include "Dom/JsonObject.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	const TCHAR* const ReloadSpec_TransientSourceName = TEXT("ClaireonSpecGameplayTags.ini");

	FString ReloadSpec_TransientDir()
	{
		return FPaths::ProjectIntermediateDir() / TEXT("ClaireonSpecs");
	}

	FString ReloadSpec_TransientFile()
	{
		return ReloadSpec_TransientDir() / ReloadSpec_TransientSourceName;
	}

	void ReloadSpec_RegisterTransientSource()
	{
		IGameplayTagsEditorModule::Get().AddNewGameplayTagSource(
			ReloadSpec_TransientSourceName,
			ReloadSpec_TransientDir());
	}

	void ReloadSpec_Cleanup()
	{
		IFileManager::Get().Delete(*ReloadSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
		UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();
	}
}

// =====================================================================================
// Test 1: Reload picks up an out-of-band ini edit (file written directly via IPlatformFile,
// without calling the manager API).
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsReloadTest_OutOfBandEdit,
	"Claireon.GameplayTagsReload.OutOfBandEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsReloadTest_OutOfBandEdit::RunTest(const FString& /*Parameters*/)
{
	// BeforeEach: register transient source. Existing file from prior aborted run is removed.
	IFileManager::Get().Delete(*ReloadSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
	ReloadSpec_RegisterTransientSource();

	const FString ExternalTagName = TEXT("Claireon.Spec.Reload.External");

	// Sanity: tag should not currently be valid.
	UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
	{
		const FGameplayTag PreReloadTag = Manager.RequestGameplayTag(FName(*ExternalTagName), /*ErrorIfNotFound*/false);
		if (PreReloadTag.IsValid())
		{
			AddError(FString::Printf(TEXT("Pre-condition failed: tag '%s' already valid before out-of-band write"), *ExternalTagName));
			ReloadSpec_Cleanup();
			return false;
		}
	}

	// Write the ini row directly via IPlatformFile (simulating an external text-editor write).
	const FString IniContents = FString::Printf(
		TEXT("[/Script/GameplayTags.GameplayTagsList]\r\n+GameplayTagList=(Tag=\"%s\",DevComment=\"\")\r\n"),
		*ExternalTagName);
	if (!FFileHelper::SaveStringToFile(IniContents, *ReloadSpec_TransientFile()))
	{
		AddError(FString::Printf(TEXT("Failed to write transient ini at %s"), *ReloadSpec_TransientFile()));
		ReloadSpec_Cleanup();
		return false;
	}

	// Before reload: the tag still isn't visible (manager hasn't re-read the file).
	{
		const FGameplayTag PreReloadTag = Manager.RequestGameplayTag(FName(*ExternalTagName), /*ErrorIfNotFound*/false);
		if (PreReloadTag.IsValid())
		{
			AddError(TEXT("Tag became valid before reload was invoked - cache invariant broken."));
			ReloadSpec_Cleanup();
			return false;
		}
	}

	// Call the reload tool.
	ClaireonTool_GameplayTagsReload ReloadTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	const IClaireonTool::FToolResult ReloadResult = ReloadTool.Execute(Args);
	if (ReloadResult.bIsError)
	{
		AddError(FString::Printf(TEXT("gameplay_tags_reload returned error: %s"), *ReloadResult.ErrorMessage));
		ReloadSpec_Cleanup();
		return false;
	}

	// After reload: the tag must now be valid.
	{
		const FGameplayTag PostReloadTag = Manager.RequestGameplayTag(FName(*ExternalTagName), /*ErrorIfNotFound*/false);
		if (!PostReloadTag.IsValid())
		{
			AddError(FString::Printf(TEXT("Tag '%s' still invalid after reload."), *ExternalTagName));
			ReloadSpec_Cleanup();
			return false;
		}
	}

	// AfterEach.
	ReloadSpec_Cleanup();
	return true;
}

// =====================================================================================
// Test 2: Reload returns the correct source path.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsReloadTest_SourcePath,
	"Claireon.GameplayTagsReload.SourcePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsReloadTest_SourcePath::RunTest(const FString& /*Parameters*/)
{
	ClaireonTool_GameplayTagsReload ReloadTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	const IClaireonTool::FToolResult Result = ReloadTool.Execute(Args);

	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("gameplay_tags_reload returned error: %s"), *Result.ErrorMessage));
		return false;
	}

	if (!Result.Data.IsValid())
	{
		AddError(TEXT("gameplay_tags_reload returned no Data payload."));
		return false;
	}

	bool bRefreshed = false;
	if (!Result.Data->TryGetBoolField(TEXT("refreshed"), bRefreshed) || !bRefreshed)
	{
		AddError(TEXT("gameplay_tags_reload result missing 'refreshed: true'."));
		return false;
	}

	FString Source;
	if (!Result.Data->TryGetStringField(TEXT("source"), Source) || Source.IsEmpty())
	{
		AddError(TEXT("gameplay_tags_reload result missing non-empty 'source'."));
		return false;
	}

	if (!Source.EndsWith(TEXT("DefaultGameplayTags.ini")))
	{
		AddError(FString::Printf(TEXT("Reported source '%s' does not end with DefaultGameplayTags.ini"), *Source));
		return false;
	}

	return true;
}
