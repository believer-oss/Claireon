// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_MapStatus.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

FString ClaireonTool_MapStatus::GetName() const
{
	return TEXT("get_map_status");
}

FString ClaireonTool_MapStatus::GetCategory() const
{
	return TEXT("level");
}

FString ClaireonTool_MapStatus::GetDescription() const
{
	return TEXT("Get the currently loaded map name, path, and load state");
}

TSharedPtr<FJsonObject> ClaireonTool_MapStatus::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_MapStatus::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return MakeErrorResult(TEXT("No world loaded"));
	}

	const FString MapName = World->GetMapName();
	const FString MapPath = World->GetPathName();

	// Count actors
	int32 ActorCount = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		++ActorCount;
	}

	// Dirty flag
	const bool bDirty = World->GetPackage() ? World->GetPackage()->IsDirty() : false;

	// Sub-levels
	TArray<TSharedPtr<FJsonValue>> SubLevelsArray;
	for (ULevelStreaming* StreamingLevel : World->GetStreamingLevels())
	{
		if (StreamingLevel)
		{
			SubLevelsArray.Add(MakeShared<FJsonValueString>(StreamingLevel->GetWorldAssetPackageName()));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("map_name"), MapName);
	Data->SetStringField(TEXT("map_path"), MapPath);
	Data->SetNumberField(TEXT("actor_count"), ActorCount);
	Data->SetBoolField(TEXT("dirty"), bDirty);
	Data->SetArrayField(TEXT("sub_levels"), SubLevelsArray);

	const FString Summary = FString::Printf(TEXT("%s: %d actors, %d sub-levels"),
		*MapName, ActorCount, SubLevelsArray.Num());

	return MakeSuccessResult(Data, Summary);
}
