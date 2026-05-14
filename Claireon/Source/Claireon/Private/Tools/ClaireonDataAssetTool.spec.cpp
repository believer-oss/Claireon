// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonDataAssetTool_Create.h"
#include "Tools/ClaireonDeveloperSettingsTool_Get.h"
#include "Tools/ClaireonTool_AssetExists.h"
#include "Tools/ClaireonPropertyUtils.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Misc/AutomationTest.h"
#include "UObject/Object.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	// DataAssetSpec_: discriminator-prefixed anon-NS helpers (CLAUDE.md unity-collision guideline).

	void DataAssetSpec_DeleteIfExists(const FString& Path)
	{
		if (UEditorAssetLibrary::DoesAssetExist(Path))
		{
			UEditorAssetLibrary::DeleteAsset(Path);
		}
	}

	IClaireonTool::FToolResult DataAssetSpec_RunCreate(
		const FString& AssetPath,
		const FString& ClassPath,
		TSharedPtr<FJsonObject> Properties /* may be nullptr */)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("class_path"), ClassPath);
		if (Properties.IsValid())
		{
			Args->SetObjectField(TEXT("properties"), Properties);
		}
		FClaireonDataAssetTool_Create Tool;
		return Tool.Execute(Args);
	}

	IClaireonTool::FToolResult DataAssetSpec_RunExists(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		ClaireonTool_AssetExists Tool;
		return Tool.Execute(Args);
	}

	IClaireonTool::FToolResult DataAssetSpec_RunDevSettings(const FString& ClassPath)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("class_path"), ClassPath);
		FClaireonDeveloperSettingsTool_Get Tool;
		return Tool.Execute(Args);
	}
} // namespace

// =====================================================================================
// Test: Create_HappyPath
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonDataAsset_Create_HappyPath,
	"Claireon.DataAssetTool.Create_HappyPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonDataAsset_Create_HappyPath::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/VOLM_ClaireonSpec_Happy");
	DataAssetSpec_DeleteIfExists(Path);

	const auto Result = DataAssetSpec_RunCreate(Path, TEXT("/Script/Engine.DataAsset"), nullptr);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Create returned error: %s"), *Result.ErrorMessage));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *Path);
	if (!Asset)
	{
		AddError(TEXT("Asset not found after Create"));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}
	const FString ActualClassPath = Asset->GetClass()->GetPathName();
	if (ActualClassPath != TEXT("/Script/Engine.DataAsset"))
	{
		AddError(FString::Printf(TEXT("Expected class /Script/Engine.DataAsset; got %s"), *ActualClassPath));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	DataAssetSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: Create_BareClassName
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonDataAsset_Create_BareClassName,
	"Claireon.DataAssetTool.Create_BareClassName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonDataAsset_Create_BareClassName::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/VOLM_ClaireonSpec_BareName");
	DataAssetSpec_DeleteIfExists(Path);

	const auto Result = DataAssetSpec_RunCreate(Path, TEXT("DataAsset"), nullptr);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Create returned error: %s"), *Result.ErrorMessage));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *Path);
	if (!Asset)
	{
		AddError(TEXT("Asset not found after Create"));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	DataAssetSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: Create_WithProperties
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonDataAsset_Create_WithProperties,
	"Claireon.DataAssetTool.Create_WithProperties",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonDataAsset_Create_WithProperties::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/VOLM_ClaireonSpec_WithProps");
	DataAssetSpec_DeleteIfExists(Path);

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("Description"), TEXT("Spec caption"));

	const auto Result = DataAssetSpec_RunCreate(Path, TEXT("/Script/Engine.DataAsset"), Props);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Create returned error: %s"), *Result.ErrorMessage));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* PropertiesSet = nullptr;
	if (!Result.Data.IsValid() || !Result.Data->TryGetArrayField(TEXT("properties_set"), PropertiesSet) || !PropertiesSet)
	{
		AddError(TEXT("Result missing properties_set array"));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}
	bool bFound = false;
	for (const TSharedPtr<FJsonValue>& V : *PropertiesSet)
	{
		FString S;
		if (V.IsValid() && V->TryGetString(S) && S == TEXT("Description")) { bFound = true; break; }
	}
	if (!bFound)
	{
		AddError(TEXT("properties_set did not contain 'Description'"));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *Path);
	if (!Asset)
	{
		AddError(TEXT("Asset not found after Create"));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}
	FString ReadError;
	const FString ReadBack = ClaireonPropertyUtils::ReadPropertyByPath(Asset, TEXT("Description"), ReadError);
	if (!ReadError.IsEmpty())
	{
		AddError(FString::Printf(TEXT("ReadPropertyByPath error: %s"), *ReadError));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}
	if (ReadBack != TEXT("Spec caption"))
	{
		AddError(FString::Printf(TEXT("Description read-back mismatch: expected 'Spec caption', got '%s'"), *ReadBack));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	DataAssetSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: Create_WithSoftRef
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonDataAsset_Create_WithSoftRef,
	"Claireon.DataAssetTool.Create_WithSoftRef",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonDataAsset_Create_WithSoftRef::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/VOLM_ClaireonSpec_SoftRef");
	DataAssetSpec_DeleteIfExists(Path);

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("Speaker"), TEXT("/Game/Tests/VOSpk_NoExist.VOSpk_NoExist"));

	const auto Result = DataAssetSpec_RunCreate(Path, TEXT("/Script/Engine.DataAsset"), Props);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Create returned error: %s"), *Result.ErrorMessage));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *Path);
	if (!Asset)
	{
		AddError(TEXT("Asset not found after Create"));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}
	FString ReadError;
	const FString ReadBack = ClaireonPropertyUtils::ReadPropertyByPath(Asset, TEXT("Speaker"), ReadError);
	if (!ReadError.IsEmpty())
	{
		AddError(FString::Printf(TEXT("ReadPropertyByPath error: %s"), *ReadError));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}
	if (!ReadBack.Contains(TEXT("/Game/Tests/VOSpk_NoExist")))
	{
		AddError(FString::Printf(TEXT("Speaker read-back missing expected fragment: got '%s'"), *ReadBack));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	DataAssetSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: Create_DuplicatePathError
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonDataAsset_Create_DuplicatePathError,
	"Claireon.DataAssetTool.Create_DuplicatePathError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonDataAsset_Create_DuplicatePathError::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/VOLM_ClaireonSpec_Dup");
	DataAssetSpec_DeleteIfExists(Path);

	const auto First = DataAssetSpec_RunCreate(Path, TEXT("/Script/Engine.DataAsset"), nullptr);
	if (First.bIsError)
	{
		AddError(FString::Printf(TEXT("First Create returned error: %s"), *First.ErrorMessage));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	const auto Second = DataAssetSpec_RunCreate(Path, TEXT("/Script/Engine.DataAsset"), nullptr);
	if (!Second.bIsError)
	{
		AddError(TEXT("Second Create unexpectedly succeeded"));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}
	if (!Second.ErrorMessage.Contains(TEXT("Asset already exists")))
	{
		AddError(FString::Printf(TEXT("Second Create error does not mention 'Asset already exists': %s"), *Second.ErrorMessage));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	DataAssetSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: Create_NonDataAssetClass
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonDataAsset_Create_NonDataAssetClass,
	"Claireon.DataAssetTool.Create_NonDataAssetClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonDataAsset_Create_NonDataAssetClass::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/VOLM_ClaireonSpec_NotDataAsset");
	DataAssetSpec_DeleteIfExists(Path);

	const auto Result = DataAssetSpec_RunCreate(Path, TEXT("/Script/Engine.Texture2D"), nullptr);
	if (!Result.bIsError)
	{
		AddError(TEXT("Create unexpectedly succeeded for non-UDataAsset class"));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}
	if (!Result.ErrorMessage.Contains(TEXT("not a UDataAsset subclass")))
	{
		AddError(FString::Printf(TEXT("Error message missing 'not a UDataAsset subclass': %s"), *Result.ErrorMessage));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	DataAssetSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: Create_UnresolvableClass
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonDataAsset_Create_UnresolvableClass,
	"Claireon.DataAssetTool.Create_UnresolvableClass",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonDataAsset_Create_UnresolvableClass::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/VOLM_ClaireonSpec_Unresolvable");
	DataAssetSpec_DeleteIfExists(Path);

	const auto Result = DataAssetSpec_RunCreate(Path, TEXT("NotARealClass_xyz123"), nullptr);
	if (!Result.bIsError)
	{
		AddError(TEXT("Create unexpectedly succeeded for unresolvable class name"));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}
	if (!Result.ErrorMessage.Contains(TEXT("Could not resolve class")))
	{
		AddError(FString::Printf(TEXT("Error message missing 'Could not resolve class': %s"), *Result.ErrorMessage));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	return true;
}

// =====================================================================================
// Test: Create_PropertyWriteFailureRollback
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonDataAsset_Create_PropertyWriteFailureRollback,
	"Claireon.DataAssetTool.Create_PropertyWriteFailureRollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonDataAsset_Create_PropertyWriteFailureRollback::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/VOLM_ClaireonSpec_Rollback");
	DataAssetSpec_DeleteIfExists(Path);

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("NonExistentField"), TEXT("x"));

	const auto Result = DataAssetSpec_RunCreate(Path, TEXT("/Script/Engine.DataAsset"), Props);
	if (!Result.bIsError)
	{
		AddError(TEXT("Create unexpectedly succeeded for non-existent property"));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}
	if (!Result.ErrorMessage.Contains(TEXT("Failed to set property")))
	{
		AddError(FString::Printf(TEXT("Error message missing 'Failed to set property': %s"), *Result.ErrorMessage));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}
	if (!Result.ErrorMessage.Contains(TEXT("NonExistentField")))
	{
		AddError(FString::Printf(TEXT("Error message missing 'NonExistentField': %s"), *Result.ErrorMessage));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	UObject* PostFailure = LoadObject<UObject>(nullptr, *Path);
	if (PostFailure != nullptr)
	{
		AddError(TEXT("Half-built asset still visible via LoadObject after property-write failure (rollback failed)"));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	return true;
}

// =====================================================================================
// Test: AssetExists_Positive
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonAssetExists_Positive,
	"Claireon.DataAssetTool.AssetExists_Positive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonAssetExists_Positive::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/VOLM_ClaireonSpec_ExistsYes");
	DataAssetSpec_DeleteIfExists(Path);

	const auto Create = DataAssetSpec_RunCreate(Path, TEXT("/Script/Engine.DataAsset"), nullptr);
	if (Create.bIsError)
	{
		AddError(FString::Printf(TEXT("Create returned error: %s"), *Create.ErrorMessage));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	const auto Result = DataAssetSpec_RunExists(Path);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Exists returned error: %s"), *Result.ErrorMessage));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}
	bool bExists = false;
	if (!Result.Data.IsValid() || !Result.Data->TryGetBoolField(TEXT("exists"), bExists))
	{
		AddError(TEXT("Result missing 'exists' field"));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}
	if (!bExists)
	{
		AddError(TEXT("exists=false for an asset that was just created"));
		DataAssetSpec_DeleteIfExists(Path);
		return false;
	}

	DataAssetSpec_DeleteIfExists(Path);
	return true;
}

// =====================================================================================
// Test: AssetExists_Negative
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonAssetExists_Negative,
	"Claireon.DataAssetTool.AssetExists_Negative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonAssetExists_Negative::RunTest(const FString& /*Parameters*/)
{
	const FString Path = TEXT("/Game/Tests/VOLM_NonExistent_zzz");
	DataAssetSpec_DeleteIfExists(Path);

	const auto Result = DataAssetSpec_RunExists(Path);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Exists returned error: %s"), *Result.ErrorMessage));
		return false;
	}
	bool bExists = true;
	if (!Result.Data.IsValid() || !Result.Data->TryGetBoolField(TEXT("exists"), bExists))
	{
		AddError(TEXT("Result missing 'exists' field"));
		return false;
	}
	if (bExists)
	{
		AddError(TEXT("exists=true for a non-existent asset"));
		return false;
	}

	return true;
}

// =====================================================================================
// Test: AssetExists_InvalidPath
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonAssetExists_InvalidPath,
	"Claireon.DataAssetTool.AssetExists_InvalidPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonAssetExists_InvalidPath::RunTest(const FString& /*Parameters*/)
{
	const auto Result = DataAssetSpec_RunExists(TEXT("/NotGame/Foo"));
	if (!Result.bIsError)
	{
		AddError(TEXT("Exists unexpectedly succeeded for /NotGame/ path"));
		return false;
	}
	if (!Result.ErrorMessage.Contains(TEXT("/Game/")))
	{
		AddError(FString::Printf(TEXT("Error message missing '/Game/': %s"), *Result.ErrorMessage));
		return false;
	}
	return true;
}

// =====================================================================================
// Test: DevSettings_Get_EditorStyleSettings
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonDevSettings_Get_EditorStyleSettings,
	"Claireon.DataAssetTool.DevSettings_Get_EditorStyleSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonDevSettings_Get_EditorStyleSettings::RunTest(const FString& /*Parameters*/)
{
	const auto Result = DataAssetSpec_RunDevSettings(TEXT("/Script/EditorStyle.EditorStyleSettings"));
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("DevSettings returned error: %s"), *Result.ErrorMessage));
		return false;
	}
	if (!Result.Data.IsValid())
	{
		AddError(TEXT("Result.Data is null"));
		return false;
	}

	FString ClassName, ClassPath, ConfigSection;
	if (!Result.Data->TryGetStringField(TEXT("class_name"), ClassName) || ClassName != TEXT("EditorStyleSettings"))
	{
		AddError(FString::Printf(TEXT("class_name mismatch: got '%s'"), *ClassName));
		return false;
	}
	if (!Result.Data->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath != TEXT("/Script/EditorStyle.EditorStyleSettings"))
	{
		AddError(FString::Printf(TEXT("class_path mismatch: got '%s'"), *ClassPath));
		return false;
	}
	if (!Result.Data->TryGetStringField(TEXT("config_section"), ConfigSection) || ConfigSection != TEXT("Game"))
	{
		AddError(FString::Printf(TEXT("config_section mismatch: expected 'Game', got '%s'"), *ConfigSection));
		return false;
	}

	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (!Result.Data->TryGetObjectField(TEXT("properties"), PropsObj) || !PropsObj || !PropsObj->IsValid())
	{
		AddError(TEXT("properties object missing"));
		return false;
	}
	if (!(*PropsObj)->HasField(TEXT("ResolutionChooser")))
	{
		AddError(TEXT("properties missing key 'ResolutionChooser'"));
		return false;
	}

	return true;
}

// =====================================================================================
// Test: DevSettings_Get_NotDeveloperSettings
// =====================================================================================
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FClaireonDevSettings_Get_NotDeveloperSettings,
	"Claireon.DataAssetTool.DevSettings_Get_NotDeveloperSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FClaireonDevSettings_Get_NotDeveloperSettings::RunTest(const FString& /*Parameters*/)
{
	const auto Result = DataAssetSpec_RunDevSettings(TEXT("/Script/Engine.Texture2D"));
	if (!Result.bIsError)
	{
		AddError(TEXT("DevSettings unexpectedly succeeded for non-UDeveloperSettings class"));
		return false;
	}
	if (!Result.ErrorMessage.Contains(TEXT("not a UDeveloperSettings subclass")))
	{
		AddError(FString::Printf(TEXT("Error message missing 'not a UDeveloperSettings subclass': %s"), *Result.ErrorMessage));
		return false;
	}
	return true;
}
