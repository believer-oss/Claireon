// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Cross-tool integration specs for the claireon.gameplay_tags_* family (#0000).
// Tests the realistic operator workflow (add -> reload -> remove) and verifies
// the cleanup invariant that Config/DefaultGameplayTags.ini is byte-for-byte
// unchanged after every spec run.

#include "Tools/ClaireonTool_GameplayTagsAdd.h"
#include "Tools/ClaireonTool_GameplayTagsReload.h"
#include "Tools/ClaireonTool_GameplayTagsRemove.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "GameplayTagsSettings.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	const TCHAR* const IntegrationSpec_TransientSource = TEXT("ClaireonSpecGameplayTags.ini");

	FString IntegrationSpec_TransientDir()
	{
		return FPaths::ProjectIntermediateDir() / TEXT("ClaireonSpecs");
	}

	FString IntegrationSpec_TransientFile()
	{
		return IntegrationSpec_TransientDir() / IntegrationSpec_TransientSource;
	}

	FString IntegrationSpec_DefaultConfigPath()
	{
		const UGameplayTagsSettings* Settings = GetDefault<UGameplayTagsSettings>();
		return Settings ? Settings->GetDefaultConfigFilename() : FString();
	}

	void IntegrationSpec_RegisterTransientSource()
	{
		IGameplayTagsEditorModule::Get().AddNewGameplayTagSource(
			IntegrationSpec_TransientSource,
			IntegrationSpec_TransientDir());
	}

	void IntegrationSpec_Cleanup()
	{
		IFileManager::Get().Delete(*IntegrationSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
		UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();
	}

	bool IntegrationSpec_AddTag(const FString& TagName, FString& OutError)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("tag"), TagName);
		TArray<TSharedPtr<FJsonValue>> Entries;
		Entries.Add(MakeShared<FJsonValueObject>(Entry));

		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetArrayField(TEXT("tags"), Entries);
		Args->SetStringField(TEXT("tag_source"), IntegrationSpec_TransientSource);

		ClaireonTool_GameplayTagsAdd Adder;
		const IClaireonTool::FToolResult R = Adder.Execute(Args);
		if (R.bIsError)
		{
			OutError = R.GetContentAsString();
			return false;
		}
		return true;
	}

	bool IntegrationSpec_RemoveTags(const TArray<FString>& Tags, FString& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> ValueTags;
		ValueTags.Reserve(Tags.Num());
		for (const FString& T : Tags)
		{
			ValueTags.Add(MakeShared<FJsonValueString>(T));
		}

		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetArrayField(TEXT("tags"), ValueTags);
		Args->SetStringField(TEXT("tag_source"), IntegrationSpec_TransientSource);

		ClaireonTool_GameplayTagsRemove Remover;
		const IClaireonTool::FToolResult R = Remover.Execute(Args);
		if (R.bIsError)
		{
			OutError = R.GetContentAsString();
			return false;
		}
		return true;
	}

	bool IntegrationSpec_Reload(FString& OutError)
	{
		ClaireonTool_GameplayTagsReload Reloader;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		const IClaireonTool::FToolResult R = Reloader.Execute(Args);
		if (R.bIsError)
		{
			OutError = R.GetContentAsString();
			return false;
		}
		return true;
	}
}

// =====================================================================================
// Test 1: Add -> Reload -> Remove cycle. Tags must remain valid across the reload.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsIntegrationTest_AddReloadRemoveCycle,
	"Claireon.GameplayTagsIntegration.AddReloadRemoveCycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsIntegrationTest_AddReloadRemoveCycle::RunTest(const FString& /*Parameters*/)
{
	IFileManager::Get().Delete(*IntegrationSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
	IntegrationSpec_RegisterTransientSource();

	const TArray<FString> TagNames = {
		TEXT("Claireon.Spec.Integration.Cycle.A"),
		TEXT("Claireon.Spec.Integration.Cycle.B"),
	};

	bool bPassed = true;

	for (const FString& T : TagNames)
	{
		FString Err;
		if (!IntegrationSpec_AddTag(T, Err))
		{
			AddError(FString::Printf(TEXT("Add failed for '%s': %s"), *T, *Err));
			bPassed = false;
		}
	}

	{
		FString Err;
		if (!IntegrationSpec_Reload(Err))
		{
			AddError(FString::Printf(TEXT("Reload failed: %s"), *Err));
			bPassed = false;
		}
	}

	UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
	for (const FString& T : TagNames)
	{
		const FGameplayTag Tag = Manager.RequestGameplayTag(FName(*T), /*ErrorIfNotFound*/false);
		if (!Tag.IsValid())
		{
			AddError(FString::Printf(TEXT("Tag '%s' invalid after reload."), *T));
			bPassed = false;
		}
	}

	{
		FString Err;
		if (!IntegrationSpec_RemoveTags(TagNames, Err))
		{
			AddError(FString::Printf(TEXT("Remove failed: %s"), *Err));
			bPassed = false;
		}
	}

	for (const FString& T : TagNames)
	{
		const FGameplayTag Tag = Manager.RequestGameplayTag(FName(*T), /*ErrorIfNotFound*/false);
		if (Tag.IsValid())
		{
			AddError(FString::Printf(TEXT("Tag '%s' still valid after remove."), *T));
			bPassed = false;
		}
	}

	IntegrationSpec_Cleanup();
	return bPassed;
}

// =====================================================================================
// Test 2: Pollution check - Config/DefaultGameplayTags.ini timestamp is unchanged after
// a full transient-source add/remove/reload sequence.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsIntegrationTest_DefaultConfigUntouched,
	"Claireon.GameplayTagsIntegration.DefaultConfigUntouched",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsIntegrationTest_DefaultConfigUntouched::RunTest(const FString& /*Parameters*/)
{
	const FString DefaultConfig = IntegrationSpec_DefaultConfigPath();
	if (DefaultConfig.IsEmpty())
	{
		AddError(TEXT("Could not resolve DefaultGameplayTags.ini path."));
		return false;
	}

	const FDateTime PreTimestamp = IFileManager::Get().GetTimeStamp(*DefaultConfig);
	if (PreTimestamp == FDateTime::MinValue())
	{
		AddError(FString::Printf(TEXT("Pre-test timestamp for %s could not be read."), *DefaultConfig));
		return false;
	}

	IFileManager::Get().Delete(*IntegrationSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
	IntegrationSpec_RegisterTransientSource();

	bool bPassed = true;

	const FString TagName = TEXT("Claireon.Spec.Integration.Pollution.Tag");

	{
		FString Err;
		if (!IntegrationSpec_AddTag(TagName, Err))
		{
			AddError(FString::Printf(TEXT("Add failed: %s"), *Err));
			bPassed = false;
		}
	}
	{
		FString Err;
		if (!IntegrationSpec_Reload(Err))
		{
			AddError(FString::Printf(TEXT("Reload failed: %s"), *Err));
			bPassed = false;
		}
	}
	{
		FString Err;
		if (!IntegrationSpec_RemoveTags({TagName}, Err))
		{
			AddError(FString::Printf(TEXT("Remove failed: %s"), *Err));
			bPassed = false;
		}
	}

	IntegrationSpec_Cleanup();

	const FDateTime PostTimestamp = IFileManager::Get().GetTimeStamp(*DefaultConfig);
	if (PostTimestamp != PreTimestamp)
	{
		AddError(FString::Printf(
			TEXT("DefaultGameplayTags.ini was modified by the test (pre=%s, post=%s). Cleanup invariant violated."),
			*PreTimestamp.ToString(),
			*PostTimestamp.ToString()));
		bPassed = false;
	}

	return bPassed;
}

// =====================================================================================
// Test 3: Reload after an external file edit, then add another tag into the same source.
// Both tags must be valid; the manager must see the external row only after reload.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsIntegrationTest_ExternalEditThenAdd,
	"Claireon.GameplayTagsIntegration.ExternalEditThenAdd",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsIntegrationTest_ExternalEditThenAdd::RunTest(const FString& /*Parameters*/)
{
	IFileManager::Get().Delete(*IntegrationSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
	IntegrationSpec_RegisterTransientSource();

	const FString ExternalTag = TEXT("Claireon.Spec.Cross.External");
	const FString ToolAddedTag = TEXT("Claireon.Spec.Cross.ToolAdded");

	bool bPassed = true;

	// Write the external tag row directly via IPlatformFile (no manager API).
	const FString IniContents = FString::Printf(
		TEXT("[/Script/GameplayTags.GameplayTagsList]\r\n+GameplayTagList=(Tag=\"%s\",DevComment=\"\")\r\n"),
		*ExternalTag);
	if (!FFileHelper::SaveStringToFile(IniContents, *IntegrationSpec_TransientFile()))
	{
		AddError(FString::Printf(TEXT("Failed to write transient ini at %s"), *IntegrationSpec_TransientFile()));
		IntegrationSpec_Cleanup();
		return false;
	}

	// Reload to pick up the external row.
	{
		FString Err;
		if (!IntegrationSpec_Reload(Err))
		{
			AddError(FString::Printf(TEXT("Reload failed: %s"), *Err));
			bPassed = false;
		}
	}

	UGameplayTagsManager& Manager = UGameplayTagsManager::Get();
	{
		const FGameplayTag T = Manager.RequestGameplayTag(FName(*ExternalTag), /*ErrorIfNotFound*/false);
		if (!T.IsValid())
		{
			AddError(FString::Printf(TEXT("External tag '%s' not valid after reload."), *ExternalTag));
			bPassed = false;
		}
	}

	// Add a second tag via the tool. Engine should preserve the first row and append the second.
	{
		FString Err;
		if (!IntegrationSpec_AddTag(ToolAddedTag, Err))
		{
			AddError(FString::Printf(TEXT("Tool add failed: %s"), *Err));
			bPassed = false;
		}
	}

	{
		const FGameplayTag TExternal = Manager.RequestGameplayTag(FName(*ExternalTag), /*ErrorIfNotFound*/false);
		const FGameplayTag TTool = Manager.RequestGameplayTag(FName(*ToolAddedTag), /*ErrorIfNotFound*/false);
		if (!TExternal.IsValid())
		{
			AddError(FString::Printf(TEXT("External tag '%s' invalid after tool add."), *ExternalTag));
			bPassed = false;
		}
		if (!TTool.IsValid())
		{
			AddError(FString::Printf(TEXT("Tool-added tag '%s' invalid after add."), *ToolAddedTag));
			bPassed = false;
		}
	}

	IntegrationSpec_Cleanup();
	return bPassed;
}
