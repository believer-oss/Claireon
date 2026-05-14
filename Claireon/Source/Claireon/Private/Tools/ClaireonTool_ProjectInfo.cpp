// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ProjectInfo.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

FString ClaireonTool_ProjectInfo::GetCategory() const { return TEXT("project"); }
FString ClaireonTool_ProjectInfo::GetOperation() const { return TEXT("info"); }

FString ClaireonTool_ProjectInfo::GetDescription() const
{
	return TEXT("Get project name, modules, plugins, and engine association");
}

TSharedPtr<FJsonObject> ClaireonTool_ProjectInfo::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_ProjectInfo::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	const FString ProjectName = FApp::GetProjectName();

	// Read the .uproject file for module/plugin info
	const FString UProjectPath = FPaths::GetProjectFilePath();
	TArray<TSharedPtr<FJsonValue>> ModulesArray;
	TArray<TSharedPtr<FJsonValue>> PluginsArray;
	TArray<TSharedPtr<FJsonValue>> TargetPlatformsArray;
	TArray<TSharedPtr<FJsonValue>> ContentPathsArray;

	FString UProjectContent;
	if (FFileHelper::LoadFileToString(UProjectContent, *UProjectPath))
	{
		TSharedPtr<FJsonObject> UProjectJson;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(UProjectContent);
		if (FJsonSerializer::Deserialize(Reader, UProjectJson) && UProjectJson.IsValid())
		{
			// Modules
			const TArray<TSharedPtr<FJsonValue>>* ModulesJsonArray = nullptr;
			if (UProjectJson->TryGetArrayField(TEXT("Modules"), ModulesJsonArray))
			{
				for (const TSharedPtr<FJsonValue>& ModVal : *ModulesJsonArray)
				{
					const TSharedPtr<FJsonObject>* ModObj = nullptr;
					if (ModVal->TryGetObject(ModObj))
					{
						FString ModName;
						(*ModObj)->TryGetStringField(TEXT("Name"), ModName);
						ModulesArray.Add(MakeShared<FJsonValueString>(ModName));
					}
				}
			}

			// Plugins
			const TArray<TSharedPtr<FJsonValue>>* PluginsJsonArray = nullptr;
			if (UProjectJson->TryGetArrayField(TEXT("Plugins"), PluginsJsonArray))
			{
				for (const TSharedPtr<FJsonValue>& PlugVal : *PluginsJsonArray)
				{
					const TSharedPtr<FJsonObject>* PlugObj = nullptr;
					if (PlugVal->TryGetObject(PlugObj))
					{
						FString PlugName;
						bool bEnabled = false;
						(*PlugObj)->TryGetStringField(TEXT("Name"), PlugName);
						(*PlugObj)->TryGetBoolField(TEXT("Enabled"), bEnabled);
						if (bEnabled)
						{
							PluginsArray.Add(MakeShared<FJsonValueString>(PlugName));
						}
					}
				}
			}

			// Target platforms
			const TArray<TSharedPtr<FJsonValue>>* PlatformsJsonArray = nullptr;
			if (UProjectJson->TryGetArrayField(TEXT("TargetPlatforms"), PlatformsJsonArray))
			{
				for (const TSharedPtr<FJsonValue>& PlatVal : *PlatformsJsonArray)
				{
					TargetPlatformsArray.Add(PlatVal);
				}
			}
		}
	}

	// Content paths
	ContentPathsArray.Add(MakeShared<FJsonValueString>(FPaths::ProjectContentDir()));

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("project_name"), ProjectName);
	Data->SetArrayField(TEXT("target_platforms"), TargetPlatformsArray);
	Data->SetArrayField(TEXT("modules"), ModulesArray);
	Data->SetArrayField(TEXT("plugins"), PluginsArray);
	Data->SetArrayField(TEXT("content_paths"), ContentPathsArray);

	const FString Summary = FString::Printf(TEXT("%s: %d modules, %d plugins"),
		*ProjectName, ModulesArray.Num(), PluginsArray.Num());

	return MakeSuccessResult(Data, Summary);
}
