// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonDeveloperSettingsTool_Get.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonPropertyUtils.h"

#include "Dom/JsonObject.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

namespace
{
	// DevSettingsGet_: discriminator-prefixed file-local helpers to avoid unity-batch
	// symbol collisions with similarly-shaped helpers in cohort files.

	/**
	 * Case-insensitive match of a UClass against a caller-supplied identifier.
	 *
	 * Accepted forms (all case-insensitive):
	 *   /Script/Module.ClassName   -- full object path (canonical INI section key)
	 *   ClassName                  -- bare class name (no prefix letter)
	 *   UClassName                 -- U-prefixed class name (as written in source)
	 */
	bool DevSettingsGet_ClassMatches(const UClass* Class, const FString& Identifier)
	{
		if (!Class || Identifier.IsEmpty())
		{
			return false;
		}
		if (Class->GetPathName().Equals(Identifier, ESearchCase::IgnoreCase))
		{
			return true;
		}
		const FString Name = Class->GetName();
		if (Name.Equals(Identifier, ESearchCase::IgnoreCase))
		{
			return true;
		}
		// Strip leading U/u and re-check the short name (user copied from source).
		if (Identifier.Len() > 1 && (Identifier[0] == TEXT('U') || Identifier[0] == TEXT('u')))
		{
			if (Name.Equals(Identifier.Mid(1), ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Walk every loaded UClass and return the first UDeveloperSettings subclass
	 * that matches Identifier. Using TObjectIterator finds classes in Private
	 * editor modules that are invisible to Python's unreal.* namespace.
	 */
	UClass* DevSettingsGet_ResolveClass(const FString& Identifier)
	{
		for (TObjectIterator<UClass> It; It; ++It)
		{
			UClass* Class = *It;
			if (!Class || !Class->IsChildOf(UDeveloperSettings::StaticClass()))
			{
				continue;
			}
			if (Class == UDeveloperSettings::StaticClass())
			{
				continue;
			}
			if (DevSettingsGet_ClassMatches(Class, Identifier))
			{
				return Class;
			}
		}
		return nullptr;
	}
} // namespace

FString FClaireonDeveloperSettingsTool_Get::GetCategory() const { return TEXT("developer_settings"); }
FString FClaireonDeveloperSettingsTool_Get::GetOperation() const { return TEXT("get"); }

FString FClaireonDeveloperSettingsTool_Get::GetDescription() const
{
	return TEXT(
		"Return the CDO of any UDeveloperSettings subclass as a JSON property dump (config-resolved values). "
		"class_path accepts: full object path (/Script/Module.ClassName), bare class name, "
		"U-prefixed class name (UFoo), or INI section name. "
		"Finds classes in Private editor modules that are invisible to Python by iterating all loaded UClass objects."
	);
}

TSharedPtr<FJsonObject> FClaireonDeveloperSettingsTool_Get::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("class_path"),
		TEXT("/Script/Module.ClassName, bare class name, U-prefixed name, or INI section name; must be a UDeveloperSettings subclass"),
		/*bRequired=*/true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonDeveloperSettingsTool_Get::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	FString ClassPath;
	if (!Arguments->TryGetStringField(TEXT("class_path"), ClassPath) || ClassPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: class_path"));
	}

	UClass* ResolvedClass = DevSettingsGet_ResolveClass(ClassPath);
	if (!ResolvedClass)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("No loaded UDeveloperSettings subclass matched '%s'. "
			     "Confirm the module containing the class is loaded and the name is spelled correctly."),
			*ClassPath
		));
	}

	UObject* CDO = ResolvedClass->GetDefaultObject(/*bCreateIfNeeded=*/true);
	if (!CDO)
	{
		return MakeErrorResult(FString::Printf(TEXT("GetDefaultObject returned null for %s"), *ResolvedClass->GetName()));
	}

	TSharedPtr<FJsonObject> Properties = ClaireonPropertyUtils::GetAllProperties(CDO, /*Filter=*/TEXT(""), /*Depth=*/2);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("class_path"), ResolvedClass->GetPathName());
	Data->SetStringField(TEXT("class_name"), ResolvedClass->GetName());
	Data->SetStringField(TEXT("module"), ResolvedClass->GetOutermost()->GetName());
	Data->SetStringField(TEXT("config_section"),
		ResolvedClass->ClassConfigName == NAME_None ? FString() : ResolvedClass->ClassConfigName.ToString());
	Data->SetObjectField(TEXT("properties"), Properties);

	const FString Summary = FString::Printf(TEXT("Read developer settings %s"), *ResolvedClass->GetName());
	return MakeSuccessResult(Data, Summary);
}
