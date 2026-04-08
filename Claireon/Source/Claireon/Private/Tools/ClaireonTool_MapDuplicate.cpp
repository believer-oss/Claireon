// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_MapDuplicate.h"
#include "ClaireonBridge.h"
#include "ClaireonLog.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "PackageTools.h"
#include "UObject/SavePackage.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_MapDuplicate::GetName() const
{
	return TEXT("claireon.duplicate_and_open_map_async");
}

FString ClaireonTool_MapDuplicate::GetCategory() const
{
	return TEXT("level");
}

FString ClaireonTool_MapDuplicate::GetDescription() const
{
	return TEXT("Duplicate a map asset and open the copy in the editor. "
		"The duplication and map open are deferred until after the current script finishes "
		"(world transition). Do not depend on the new map being loaded in subsequent lines "
		"of the same execute() call.");
}

TSharedPtr<FJsonObject> ClaireonTool_MapDuplicate::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> SourcePathProp = MakeShared<FJsonObject>();
	SourcePathProp->SetStringField(TEXT("type"), TEXT("string"));
	SourcePathProp->SetStringField(TEXT("description"),
		TEXT("Asset path of the source map to duplicate (e.g. /Game/Maps/L_MyLevel)"));
	Properties->SetObjectField(TEXT("source_path"), SourcePathProp);

	TSharedPtr<FJsonObject> DestPathProp = MakeShared<FJsonObject>();
	DestPathProp->SetStringField(TEXT("type"), TEXT("string"));
	DestPathProp->SetStringField(TEXT("description"),
		TEXT("Asset path for the duplicated map (e.g. /Game/Maps/L_MyLevel_Copy)"));
	Properties->SetObjectField(TEXT("destination_path"), DestPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("source_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("destination_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult ClaireonTool_MapDuplicate::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SourcePath;
	if (!Arguments->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: source_path"));
	}

	FString DestPath;
	if (!Arguments->TryGetStringField(TEXT("destination_path"), DestPath) || DestPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: destination_path"));
	}

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	// Normalize: if caller passed a package path without object name
	FString SourceObjectPath = SourcePath;
	if (!SourceObjectPath.Contains(TEXT(".")))
	{
		FString AssetName = FPaths::GetBaseFilename(SourceObjectPath);
		SourceObjectPath = SourceObjectPath + TEXT(".") + AssetName;
	}

	FString DestObjectPath = DestPath;
	if (!DestObjectPath.Contains(TEXT(".")))
	{
		FString AssetName = FPaths::GetBaseFilename(DestObjectPath);
		DestObjectPath = DestObjectPath + TEXT(".") + AssetName;
	}

	// Validate source exists
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData SourceData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(SourceObjectPath));
	if (!SourceData.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Source map not found: %s"), *SourcePath));
	}

	// Validate destination does not already exist
	FAssetData DestData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(DestObjectPath));
	if (DestData.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Destination already exists: %s"), *DestPath));
	}

	// Build JSON payload for the deferred action
	TSharedPtr<FJsonObject> PayloadObj = MakeShared<FJsonObject>();
	PayloadObj->SetStringField(TEXT("source"), SourcePath);
	PayloadObj->SetStringField(TEXT("destination"), DestPath);

	FString PayloadJson;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&PayloadJson);
	FJsonSerializer::Serialize(PayloadObj.ToSharedRef(), Writer);

	// Enqueue deferred action
	FClaireonBridge::EnqueueDeferredAction({
		EClaireonDeferredActionType::DuplicateAndOpenMap,
		PayloadJson
	});

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("deferred"));
	Data->SetStringField(TEXT("action"), TEXT("duplicate_and_open_map"));
	Data->SetStringField(TEXT("source_path"), SourcePath);
	Data->SetStringField(TEXT("destination_path"), DestPath);

	return MakeSuccessResult(Data, TEXT("Map duplication and open enqueued"));
}

void ClaireonTool_MapDuplicate::ExecuteDeferredDuplicateAndOpenMap(const FString& Payload)
{
	// Parse JSON payload
	TSharedPtr<FJsonObject> PayloadObj;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
	FJsonSerializer::Deserialize(Reader, PayloadObj);

	if (!PayloadObj.IsValid())
	{
		UE_LOG(LogClaireon, Error, TEXT("[MCP MapDuplicate] Failed to parse deferred payload: %s"), *Payload);
		return;
	}

	FString Source = PayloadObj->GetStringField(TEXT("source"));
	FString Dest = PayloadObj->GetStringField(TEXT("destination"));

	// Two-phase deferred execution:
	// Phase 1: Duplicate the asset, save it to disk, and fully unload it from memory.
	//          DuplicateAsset loads the result into memory — if we don't unload it,
	//          LoadMap's world transition sees it as a "World Memory Leak" and fatally asserts.
	// Phase 2: Load the map fresh from disk in a separate tick.
	FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateLambda([Source, Dest](float) -> bool
		{
			FClaireonBridge::RunWorldTransitionBarrier();

			// Phase 1: Duplicate, save, and unload
			UEditorAssetSubsystem* AssetSubsystem = GEditor->GetEditorSubsystem<UEditorAssetSubsystem>();
			UObject* DuplicatedAsset = AssetSubsystem->DuplicateAsset(Source, Dest);

			if (!DuplicatedAsset)
			{
				UE_LOG(LogClaireon, Error, TEXT("[MCP MapDuplicate] Failed to duplicate %s to %s"), *Source, *Dest);
				return false;
			}

			// Save the duplicated package to disk
			UPackage* Package = DuplicatedAsset->GetOutermost();
			if (Package)
			{
				FString PackageFilename;
				if (FPackageName::TryConvertLongPackageNameToFilename(Package->GetName(), PackageFilename, FPackageName::GetMapPackageExtension()))
				{
					UPackage::SavePackage(Package, DuplicatedAsset, *PackageFilename,
						FSavePackageArgs());
				}

				// Unload the package from memory so LoadMap can load it fresh
				// without triggering the world memory leak check
				TArray<UPackage*> PackagesToUnload;
				PackagesToUnload.Add(Package);
				UPackageTools::UnloadPackages(PackagesToUnload);
			}

			UE_LOG(LogClaireon, Display, TEXT("[MCP MapDuplicate] Duplicated and saved %s to %s, deferring map open to next tick"), *Source, *Dest);

			// Phase 2: Open the duplicate from disk in a separate tick
			FString MapPath = Dest;
			if (!MapPath.Contains(TEXT(".")))
			{
				FString AssetName = FPaths::GetBaseFilename(MapPath);
				MapPath = MapPath + TEXT(".") + AssetName;
			}

			FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateLambda([MapPath](float) -> bool
				{
					FClaireonBridge::RunWorldTransitionBarrier();
					FEditorFileUtils::LoadMap(MapPath);
					return false;
				}), 0.0f);

			return false; // one-shot
		}), 0.0f);
}
