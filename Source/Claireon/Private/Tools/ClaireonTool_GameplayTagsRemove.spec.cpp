// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Specs for claireon.gameplay_tags_remove. Round-trip add-then-remove
// case depends on claireon.gameplay_tags_add. Uses a transient tag source under
// FPaths::ProjectIntermediateDir() / "ClaireonSpecs" so
// Config/DefaultGameplayTags.ini is never modified.

#include "Tools/ClaireonTool_GameplayTagsAdd.h"
#include "Tools/ClaireonTool_GameplayTagsRemove.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameplayTagContainer.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
	const TCHAR* const RemoveSpec_TransientSource = TEXT("ClaireonSpecGameplayTags.ini");

	FString RemoveSpec_TransientDir()
	{
		return FPaths::ProjectIntermediateDir() / TEXT("ClaireonSpecs");
	}

	FString RemoveSpec_TransientFile()
	{
		return RemoveSpec_TransientDir() / RemoveSpec_TransientSource;
	}

	void RemoveSpec_RegisterTransientSource()
	{
		IGameplayTagsEditorModule::Get().AddNewGameplayTagSource(
			RemoveSpec_TransientSource,
			RemoveSpec_TransientDir());
	}

	void RemoveSpec_Cleanup()
	{
		IFileManager::Get().Delete(*RemoveSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
		UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();
	}

	bool RemoveSpec_AddTag(const FString& TagName, FString& OutError)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("tag"), TagName);
		TArray<TSharedPtr<FJsonValue>> Entries;
		Entries.Add(MakeShared<FJsonValueObject>(Entry));

		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetArrayField(TEXT("tags"), Entries);
		Args->SetStringField(TEXT("tag_source"), RemoveSpec_TransientSource);

		ClaireonTool_GameplayTagsAdd Adder;
		const IClaireonTool::FToolResult R = Adder.Execute(Args);
		if (R.bIsError)
		{
			OutError = R.GetContentAsString();
			return false;
		}
		return true;
	}
}

// =====================================================================================
// Test 1: Round-trip add -> remove. Depends on claireon.gameplay_tags_add.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsRemoveTest_RoundTrip,
	"Claireon.GameplayTagsRemove.RoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsRemoveTest_RoundTrip::RunTest(const FString& /*Parameters*/)
{
	IFileManager::Get().Delete(*RemoveSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
	RemoveSpec_RegisterTransientSource();

	const FString TagName = TEXT("Claireon.Spec.Remove.RoundTrip");

	FString AddErr;
	if (!RemoveSpec_AddTag(TagName, AddErr))
	{
		AddError(FString::Printf(TEXT("Add tool failed during round-trip setup: %s"), *AddErr));
		RemoveSpec_Cleanup();
		return false;
	}

	UGameplayTagsManager& Manager = UGameplayTagsManager::Get();

	bool bPassed = true;
	{
		const FGameplayTag PostAdd = Manager.RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound*/false);
		if (!PostAdd.IsValid())
		{
			AddError(FString::Printf(TEXT("Tag '%s' not valid after add."), *TagName));
			bPassed = false;
		}
	}

	// Now remove via the tool.
	{
		TArray<TSharedPtr<FJsonValue>> RemoveTags;
		RemoveTags.Add(MakeShared<FJsonValueString>(TagName));

		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetArrayField(TEXT("tags"), RemoveTags);
		Args->SetStringField(TEXT("tag_source"), RemoveSpec_TransientSource);

		ClaireonTool_GameplayTagsRemove Tool;
		const IClaireonTool::FToolResult Result = Tool.Execute(Args);
		if (Result.bIsError || !Result.Data.IsValid())
		{
			AddError(FString::Printf(TEXT("Remove returned error: %s"), *Result.GetContentAsString()));
			bPassed = false;
		}
		else
		{
			const TArray<TSharedPtr<FJsonValue>>* Removed = nullptr;
			if (!Result.Data->TryGetArrayField(TEXT("removed"), Removed) || !Removed || Removed->Num() != 1)
			{
				AddError(TEXT("Expected exactly one entry in 'removed'."));
				bPassed = false;
			}
			else
			{
				FString RemovedTag;
				if (!(*Removed)[0]->TryGetString(RemovedTag) || RemovedTag != TagName)
				{
					AddError(FString::Printf(TEXT("Unexpected 'removed' entry value: '%s'."), *RemovedTag));
					bPassed = false;
				}
			}
		}
	}

	// Assert the tag is no longer valid in the manager.
	{
		const FGameplayTag PostRemove = Manager.RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound*/false);
		if (PostRemove.IsValid())
		{
			AddError(FString::Printf(TEXT("Tag '%s' still valid after remove."), *TagName));
			bPassed = false;
		}
	}

	RemoveSpec_Cleanup();
	return bPassed;
}

// =====================================================================================
// Test 2: Removing a tag that does not exist returns a per-row failed entry, overall success.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsRemoveTest_UnknownTag,
	"Claireon.GameplayTagsRemove.UnknownTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsRemoveTest_UnknownTag::RunTest(const FString& /*Parameters*/)
{
	const FString UnknownTagName = TEXT("Claireon.Spec.Remove.DefinitelyDoesNotExist");

	TArray<TSharedPtr<FJsonValue>> Tags;
	Tags.Add(MakeShared<FJsonValueString>(UnknownTagName));

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetArrayField(TEXT("tags"), Tags);

	ClaireonTool_GameplayTagsRemove Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);

	if (Result.bIsError || !Result.Data.IsValid())
	{
		AddError(FString::Printf(TEXT("Remove returned error for unknown-tag input: %s"), *Result.GetContentAsString()));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Failed = nullptr;
	if (!Result.Data->TryGetArrayField(TEXT("failed"), Failed) || !Failed || Failed->Num() != 1)
	{
		AddError(TEXT("Expected exactly one entry in 'failed' for unknown tag."));
		return false;
	}

	const TSharedPtr<FJsonObject>* RowObj = nullptr;
	if (!(*Failed)[0]->TryGetObject(RowObj) || !RowObj)
	{
		AddError(TEXT("'failed' entry was not an object."));
		return false;
	}
	FString Reason;
	(*RowObj)->TryGetStringField(TEXT("reason"), Reason);
	if (Reason != TEXT("unknown_tag"))
	{
		AddError(FString::Printf(TEXT("Expected reason=unknown_tag, got '%s'."), *Reason));
		return false;
	}

	return true;
}
