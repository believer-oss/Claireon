// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonDeveloperSettingsTool_Get.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonPropertyUtils.h"

#include "Dom/JsonObject.h"
#include "Engine/DeveloperSettings.h"
#include "UObject/Class.h"

namespace
{
	// DevSettingsGet_: discriminator-prefixed file-local helper to avoid unity-batch collisions
	// with similarly-shaped helpers in cohort files.
	UClass* DevSettingsGet_ResolveClass(const FString& ClassPath)
	{
		if (ClassPath.StartsWith(TEXT("/Script/")))
		{
			return LoadObject<UClass>(nullptr, *ClassPath);
		}
		return ClaireonAssetUtils::ResolveClassName(ClassPath);
	}
}

FString FClaireonDeveloperSettingsTool_Get::GetCategory() const { return TEXT("developer_settings"); }
FString FClaireonDeveloperSettingsTool_Get::GetOperation() const { return TEXT("get"); }

FString FClaireonDeveloperSettingsTool_Get::GetDescription() const
{
	return TEXT("Return the CDO of any UDeveloperSettings subclass as a JSON property dump (config-resolved values). "
				"class_path accepts either /Script/Module.ClassName or a bare class name.");
}

TSharedPtr<FJsonObject> FClaireonDeveloperSettingsTool_Get::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("class_path"), TEXT("/Script/Module.ClassName or bare class name (must be a UDeveloperSettings subclass)"), true);
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
		return MakeErrorResult(FString::Printf(TEXT("Could not resolve class: %s"), *ClassPath));
	}

	if (!ResolvedClass->IsChildOf(UDeveloperSettings::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Class is not a UDeveloperSettings subclass: %s"), *ResolvedClass->GetName()));
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
	Data->SetStringField(TEXT("config_section"),
		ResolvedClass->ClassConfigName == NAME_None ? FString() : ResolvedClass->ClassConfigName.ToString());
	Data->SetObjectField(TEXT("properties"), Properties);

	const FString Summary = FString::Printf(TEXT("Read developer settings %s"), *ResolvedClass->GetName());
	return MakeSuccessResult(Data, Summary);
}
