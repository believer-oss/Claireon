// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_MapOpen.h"
#include "ClaireonLog.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "FileHelpers.h"
#include "Containers/Ticker.h"

FString ClaireonTool_MapOpen::GetName() const
{
	return TEXT("open_map");
}

FString ClaireonTool_MapOpen::GetCategory() const
{
	return TEXT("level");
}

FString ClaireonTool_MapOpen::GetDescription() const
{
	return TEXT("Open a map (level) in the editor by asset path. "
		"IMPORTANT: Always use this tool instead of unreal.EditorLevelLibrary.load_level() — "
		"raw Python map loading crashes the editor.");
}

TSharedPtr<FJsonObject> ClaireonTool_MapOpen::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// mapPath - required
	TSharedPtr<FJsonObject> MapPathProp = MakeShared<FJsonObject>();
	MapPathProp->SetStringField(TEXT("type"), TEXT("string"));
	MapPathProp->SetStringField(TEXT("description"),
		TEXT("Asset path of the map to open (e.g. /Game/Maps/MyLevel)"));
	Properties->SetObjectField(TEXT("mapPath"), MapPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("mapPath")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_MapOpen::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString MapPath;
	if (!Arguments->TryGetStringField(TEXT("mapPath"), MapPath) || MapPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: mapPath"));
	}

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	// Normalize: if caller passed a package path without object name
	// (e.g. "/Game/Maps/MyMap"), convert to full object path "/Game/Maps/MyMap.MyMap"
	if (!MapPath.Contains(TEXT(".")))
	{
		FString AssetName = FPaths::GetBaseFilename(MapPath);
		MapPath = MapPath + TEXT(".") + AssetName;
	}

	// Verify the asset exists in the registry
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData AssetData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(MapPath));
	if (!AssetData.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Map asset not found: %s"), *MapPath));
	}

	// Defer the map load to next tick (outside the current tick cycle).
	// FEditorFileUtils::LoadMap() triggers world destruction and GC, which asserts
	// if called during a tick. FTSTicker fires between frames on the game thread,
	// and LoadMap handles all post-load hooks (tab title, viewports, etc.).
	FString CapturedPath = MapPath;
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([CapturedPath](float)
		{
			FEditorFileUtils::LoadMap(CapturedPath);
			return false; // one-shot
		}), 0.0f);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("map_path"), MapPath);
	Data->SetBoolField(TEXT("success"), true);

	const FString Summary = FString::Printf(TEXT("Opening map %s"), *MapPath);
	return MakeSuccessResult(Data, Summary);
}
