// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Specs for claireon.gameplay_tags_add (#0000). Uses a transient tag source under
// FPaths::ProjectIntermediateDir() / "ClaireonSpecs" so Config/DefaultGameplayTags.ini
// is never modified.

#include "Tools/ClaireonTool_GameplayTagsAdd.h"
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
	const TCHAR* const AddSpec_TransientSource = TEXT("ClaireonSpecGameplayTags.ini");

	FString AddSpec_TransientDir()
	{
		return FPaths::ProjectIntermediateDir() / TEXT("ClaireonSpecs");
	}

	FString AddSpec_TransientFile()
	{
		return AddSpec_TransientDir() / AddSpec_TransientSource;
	}

	void AddSpec_RegisterTransientSource()
	{
		IGameplayTagsEditorModule::Get().AddNewGameplayTagSource(
			AddSpec_TransientSource,
			AddSpec_TransientDir());
	}

	void AddSpec_Cleanup()
	{
		IFileManager::Get().Delete(*AddSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
		UGameplayTagsManager::Get().EditorRefreshGameplayTagTree();
	}

	TSharedPtr<FJsonObject> AddSpec_MakeTagEntry(const FString& Tag, const FString& DevComment = FString())
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("tag"), Tag);
		if (!DevComment.IsEmpty())
		{
			Entry->SetStringField(TEXT("dev_comment"), DevComment);
		}
		return Entry;
	}

	TSharedPtr<FJsonObject> AddSpec_MakeArgs(const TArray<TSharedPtr<FJsonValue>>& Entries)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetArrayField(TEXT("tags"), Entries);
		Args->SetStringField(TEXT("tag_source"), AddSpec_TransientSource);
		return Args;
	}
}

// =====================================================================================
// Test 1: Add a single tag with a dev_comment.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsAddTest_SingleTagWithComment,
	"Claireon.GameplayTagsAdd.SingleTagWithComment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsAddTest_SingleTagWithComment::RunTest(const FString& /*Parameters*/)
{
	IFileManager::Get().Delete(*AddSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
	AddSpec_RegisterTransientSource();

	const FString TagName = TEXT("Claireon.Spec.Add.Single");

	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Add(MakeShared<FJsonValueObject>(AddSpec_MakeTagEntry(TagName, TEXT("test"))));

	ClaireonTool_GameplayTagsAdd Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(AddSpec_MakeArgs(Entries));

	bool bPassed = true;
	if (Result.bIsError || !Result.Data.IsValid())
	{
		AddError(FString::Printf(TEXT("Add returned error: %s"), *Result.GetContentAsString()));
		bPassed = false;
	}
	else
	{
		const TArray<TSharedPtr<FJsonValue>>* Added = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("added"), Added) || !Added || Added->Num() != 1)
		{
			AddError(TEXT("'added' should contain exactly one entry."));
			bPassed = false;
		}

		const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*TagName), /*ErrorIfNotFound*/false);
		if (!Tag.IsValid())
		{
			AddError(FString::Printf(TEXT("Tag '%s' is not valid in manager post-add."), *TagName));
			bPassed = false;
		}
	}

	AddSpec_Cleanup();
	return bPassed;
}

// =====================================================================================
// Test 2: Add a batch of three tags.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsAddTest_BatchOfThree,
	"Claireon.GameplayTagsAdd.BatchOfThree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsAddTest_BatchOfThree::RunTest(const FString& /*Parameters*/)
{
	IFileManager::Get().Delete(*AddSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
	AddSpec_RegisterTransientSource();

	const TArray<FString> TagNames = {
		TEXT("Claireon.Spec.Add.Batch.A"),
		TEXT("Claireon.Spec.Add.Batch.B"),
		TEXT("Claireon.Spec.Add.Batch.C"),
	};

	TArray<TSharedPtr<FJsonValue>> Entries;
	for (const FString& T : TagNames)
	{
		Entries.Add(MakeShared<FJsonValueObject>(AddSpec_MakeTagEntry(T)));
	}

	ClaireonTool_GameplayTagsAdd Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(AddSpec_MakeArgs(Entries));

	bool bPassed = true;
	if (Result.bIsError || !Result.Data.IsValid())
	{
		AddError(FString::Printf(TEXT("Add returned error: %s"), *Result.GetContentAsString()));
		bPassed = false;
	}
	else
	{
		const TArray<TSharedPtr<FJsonValue>>* Added = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("added"), Added) || !Added || Added->Num() != TagNames.Num())
		{
			AddError(FString::Printf(TEXT("'added' size = %d, expected %d."), Added ? Added->Num() : -1, TagNames.Num()));
			bPassed = false;
		}

		for (const FString& T : TagNames)
		{
			const FGameplayTag Tag = UGameplayTagsManager::Get().RequestGameplayTag(FName(*T), /*ErrorIfNotFound*/false);
			if (!Tag.IsValid())
			{
				AddError(FString::Printf(TEXT("Tag '%s' is not valid in manager."), *T));
				bPassed = false;
			}
		}
	}

	AddSpec_Cleanup();
	return bPassed;
}

// =====================================================================================
// Test 3: Add an already-present tag returns a skipped row.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsAddTest_AlreadyExists,
	"Claireon.GameplayTagsAdd.AlreadyExists",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsAddTest_AlreadyExists::RunTest(const FString& /*Parameters*/)
{
	IFileManager::Get().Delete(*AddSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
	AddSpec_RegisterTransientSource();

	const FString TagName = TEXT("Claireon.Spec.Add.Duplicate");
	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Add(MakeShared<FJsonValueObject>(AddSpec_MakeTagEntry(TagName)));

	ClaireonTool_GameplayTagsAdd Tool;

	// First add - success.
	{
		const IClaireonTool::FToolResult First = Tool.Execute(AddSpec_MakeArgs(Entries));
		if (First.bIsError)
		{
			AddError(FString::Printf(TEXT("First add failed: %s"), *First.GetContentAsString()));
			AddSpec_Cleanup();
			return false;
		}
	}

	// Second add - skipped.
	bool bPassed = true;
	{
		const IClaireonTool::FToolResult Second = Tool.Execute(AddSpec_MakeArgs(Entries));
		if (Second.bIsError || !Second.Data.IsValid())
		{
			AddError(FString::Printf(TEXT("Second add returned error: %s"), *Second.GetContentAsString()));
			bPassed = false;
		}
		else
		{
			const TArray<TSharedPtr<FJsonValue>>* Skipped = nullptr;
			if (!Second.Data->TryGetArrayField(TEXT("skipped"), Skipped) || !Skipped || Skipped->Num() != 1)
			{
				AddError(TEXT("'skipped' should contain exactly one entry."));
				bPassed = false;
			}
			else
			{
				const TSharedPtr<FJsonObject>* RowObj = nullptr;
				FString Reason;
				if (!(*Skipped)[0]->TryGetObject(RowObj) || !RowObj
					|| !(*RowObj)->TryGetStringField(TEXT("reason"), Reason)
					|| Reason != TEXT("already_exists"))
				{
					AddError(TEXT("Skipped entry should have reason=already_exists."));
					bPassed = false;
				}
			}
		}
	}

	AddSpec_Cleanup();
	return bPassed;
}

// =====================================================================================
// Test 4: Single coalesced refresh for batch add (Suspend/Resume verified by counter).
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsAddTest_CoalescedRefresh,
	"Claireon.GameplayTagsAdd.CoalescedRefresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsAddTest_CoalescedRefresh::RunTest(const FString& /*Parameters*/)
{
	IFileManager::Get().Delete(*AddSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
	AddSpec_RegisterTransientSource();

	int32 RefreshCount = 0;
	FDelegateHandle Handle = UGameplayTagsManager::OnEditorRefreshGameplayTagTree.AddLambda([&RefreshCount]()
	{
		++RefreshCount;
	});

	TArray<TSharedPtr<FJsonValue>> Entries;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		Entries.Add(MakeShared<FJsonValueObject>(AddSpec_MakeTagEntry(FString::Printf(TEXT("Claireon.Spec.Add.Coalesced.%d"), Index))));
	}

	ClaireonTool_GameplayTagsAdd Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(AddSpec_MakeArgs(Entries));

	UGameplayTagsManager::OnEditorRefreshGameplayTagTree.Remove(Handle);

	bool bPassed = true;
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Coalesced add failed: %s"), *Result.GetContentAsString()));
		bPassed = false;
	}
	if (RefreshCount != 1)
	{
		AddError(FString::Printf(TEXT("Expected exactly one OnEditorRefreshGameplayTagTree broadcast, got %d."), RefreshCount));
		bPassed = false;
	}

	AddSpec_Cleanup();
	return bPassed;
}

// =====================================================================================
// Test 5: Schema rejection - missing 'tags'.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsAddTest_MissingTags,
	"Claireon.GameplayTagsAdd.MissingTags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsAddTest_MissingTags::RunTest(const FString& /*Parameters*/)
{
	ClaireonTool_GameplayTagsAdd Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);

	if (!Result.bIsError)
	{
		AddError(TEXT("Expected error result when 'tags' is missing."));
		return false;
	}
	return true;
}

// =====================================================================================
// Test 6: Schema rejection - unknown top-level field.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsAddTest_UnknownField,
	"Claireon.GameplayTagsAdd.UnknownField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsAddTest_UnknownField::RunTest(const FString& /*Parameters*/)
{
	ClaireonTool_GameplayTagsAdd Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Empty;
	Args->SetArrayField(TEXT("tags"), Empty);
	Args->SetBoolField(TEXT("unknown_field"), true);
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);

	if (!Result.bIsError)
	{
		AddError(TEXT("Expected error result when an unknown top-level field is present."));
		return false;
	}
	return true;
}

// =====================================================================================
// Test 7: Schema rejection - non-array tags.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsAddTest_NonArrayTags,
	"Claireon.GameplayTagsAdd.NonArrayTags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsAddTest_NonArrayTags::RunTest(const FString& /*Parameters*/)
{
	ClaireonTool_GameplayTagsAdd Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("tags"), TEXT("not-an-array"));
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);

	if (!Result.bIsError)
	{
		AddError(TEXT("Expected error result when 'tags' is not an array."));
		return false;
	}
	return true;
}

// =====================================================================================
// Test 8: Invalid tag string (contains a double quote) is reported as a failed row.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonGameplayTagsAddTest_InvalidString,
	"Claireon.GameplayTagsAdd.InvalidString",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonGameplayTagsAddTest_InvalidString::RunTest(const FString& /*Parameters*/)
{
	IFileManager::Get().Delete(*AddSpec_TransientFile(), /*RequireExists*/false, /*EvenIfReadOnly*/true);
	AddSpec_RegisterTransientSource();

	const FString BadTag = TEXT("Claireon.Spec.Add.\"Bad");
	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Add(MakeShared<FJsonValueObject>(AddSpec_MakeTagEntry(BadTag)));

	ClaireonTool_GameplayTagsAdd Tool;
	const IClaireonTool::FToolResult Result = Tool.Execute(AddSpec_MakeArgs(Entries));

	bool bPassed = true;
	if (Result.bIsError || !Result.Data.IsValid())
	{
		AddError(FString::Printf(TEXT("Invalid-string add returned tool error: %s"), *Result.GetContentAsString()));
		bPassed = false;
	}
	else
	{
		const TArray<TSharedPtr<FJsonValue>>* Failed = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("failed"), Failed) || !Failed || Failed->Num() != 1)
		{
			AddError(TEXT("Expected exactly one row in 'failed'."));
			bPassed = false;
		}
		else
		{
			const TSharedPtr<FJsonObject>* Row = nullptr;
			if (!(*Failed)[0]->TryGetObject(Row) || !Row)
			{
				AddError(TEXT("'failed' row was not an object."));
				bPassed = false;
			}
			else
			{
				FString Reason, ErrorText, FixedString;
				(*Row)->TryGetStringField(TEXT("reason"), Reason);
				(*Row)->TryGetStringField(TEXT("error_text"), ErrorText);
				(*Row)->TryGetStringField(TEXT("fixed_string"), FixedString);

				if (Reason != TEXT("invalid_string"))
				{
					AddError(FString::Printf(TEXT("Expected reason=invalid_string, got '%s'."), *Reason));
					bPassed = false;
				}
				if (ErrorText.IsEmpty())
				{
					AddError(TEXT("error_text must be non-empty for invalid_string failures."));
					bPassed = false;
				}
				if (FixedString.IsEmpty())
				{
					AddError(TEXT("fixed_string must be present for invalid_string failures."));
					bPassed = false;
				}
			}
		}
	}

	AddSpec_Cleanup();
	return bPassed;
}
