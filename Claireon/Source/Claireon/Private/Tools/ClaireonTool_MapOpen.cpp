// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_MapOpen.h"
#include "ClaireonPathResolver.h"
#include "ClaireonBridge.h"
#include "ClaireonLog.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Containers/Ticker.h"

FString ClaireonTool_MapOpen::GetCategory() const { return TEXT("map"); }
FString ClaireonTool_MapOpen::GetOperation() const { return TEXT("open_async"); }

FString ClaireonTool_MapOpen::GetDescription() const
{
	return TEXT("Open a map (level) in the editor by asset path. "
		"The map load is deferred until after the current script finishes — "
		"do not depend on the map being loaded in subsequent lines of the same execute() call. "
		"A leaked-World guard runs immediately before the load: if any non-editor "
		"UWorld is still loaded in memory (e.g. you called "
		"unreal.EditorAssetLibrary.duplicate_asset() on a .umap), the load is aborted "
		"and python_execute returns bIsError with the leaked package name(s). "
		"Use duplicate_and_open_map_async to duplicate+open a map atomically.");
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

	// Resolve path to canonical form
	auto ResolveResult = ClaireonPathResolver::Resolve(MapPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	MapPath = ResolveResult.ResolvedPath.Path;

	// Post-resolver: append object name for LoadMap (required by UEditorLoadingAndSavingUtils)
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

	// Enqueue — does NOT execute yet. The post-execution hook in ExecutePython
	// will run the GC barrier then call ExecuteDeferredLoadMap.
	FClaireonBridge::EnqueueDeferredAction({
		EClaireonDeferredActionType::LoadMap,
		MapPath
	});

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("deferred"));
	Data->SetStringField(TEXT("action"), TEXT("open_map"));
	Data->SetStringField(TEXT("map_path"), MapPath);

	const FString Summary = FString::Printf(TEXT("Map load queued: %s — executes after script completes"), *MapPath);
	return MakeSuccessResult(Data, Summary);
}

void ClaireonTool_MapOpen::ExecuteDeferredLoadMap(const FString& MapPath)
{
	// Defer to next tick — LoadMap triggers world destruction and GC,
	// which asserts if called during a tick.
	FString CapturedPath = MapPath;
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([CapturedPath](float)
		{
			// Second-pass barrier: catch any UObject wrappers orphaned between
			// the post-execution barrier and this tick (e.g. from prior execute()
			// calls whose private namespaces haven't been GC'd yet).
			FClaireonBridge::RunWorldTransitionBarrier();

			// Leaked-World guard (#0000): refuse the transition if any non-editor
			// UWorld remains loaded after the barrier. Without this, LoadMap's
			// Map_Load assertion fires fatally at EditorServer.cpp:1934.
			TArray<FClaireonLeakedWorld> Remaining;
			if (!FClaireonBridge::EnsureNoLeakedWorlds(Remaining))
			{
				const FString Msg = FClaireonBridge::FormatLeakedWorldError(Remaining);
				UE_LOG(LogClaireon, Error, TEXT("%s"), *Msg);
				FClaireonBridge::ReportDeferredActionAbort(Msg);
				return false; // do NOT call LoadMap
			}

			FEditorFileUtils::LoadMap(CapturedPath);
			return false; // one-shot
		}), 0.0f);
}
