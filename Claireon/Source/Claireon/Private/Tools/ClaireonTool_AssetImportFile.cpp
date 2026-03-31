// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetImportFile.h"
#include "ClaireonLog.h"

#include "AssetToolsModule.h"
#include "AutomatedAssetImportData.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture2D.h"
#include "IAssetTools.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

FString ClaireonTool_AssetImportFile::GetName() const
{
	return TEXT("claireon.asset.import_file");
}

FString ClaireonTool_AssetImportFile::GetDescription() const
{
	return TEXT("Import a file from the local filesystem into the Unreal content browser as an asset. "
		"Supports any file type that Unreal has an import factory for (textures, meshes, audio, etc). "
		"For Texture2D imports, automatically configures UI-friendly settings (no mipmaps, TEXTUREGROUP_UI, sRGB). "
		"Returns JSON with success, asset_path, asset_type, and source_file.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetImportFile::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// source_path (required)
	TSharedPtr<FJsonObject> SourcePathProp = MakeShared<FJsonObject>();
	SourcePathProp->SetStringField(TEXT("type"), TEXT("string"));
	SourcePathProp->SetStringField(TEXT("description"),
		TEXT("Absolute filesystem path to the file to import (e.g. C:/Art/icon.png)"));
	Properties->SetObjectField(TEXT("source_path"), SourcePathProp);

	// destination_path (required)
	TSharedPtr<FJsonObject> DestPathProp = MakeShared<FJsonObject>();
	DestPathProp->SetStringField(TEXT("type"), TEXT("string"));
	DestPathProp->SetStringField(TEXT("description"),
		TEXT("Unreal content path for the destination (e.g. /Game/UI/Textures/)"));
	Properties->SetObjectField(TEXT("destination_path"), DestPathProp);

	// asset_name (optional)
	TSharedPtr<FJsonObject> AssetNameProp = MakeShared<FJsonObject>();
	AssetNameProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetNameProp->SetStringField(TEXT("description"),
		TEXT("Name for the imported asset. Defaults to the source filename without extension."));
	Properties->SetObjectField(TEXT("asset_name"), AssetNameProp);

	// overwrite (optional)
	TSharedPtr<FJsonObject> OverwriteProp = MakeShared<FJsonObject>();
	OverwriteProp->SetStringField(TEXT("type"), TEXT("boolean"));
	OverwriteProp->SetStringField(TEXT("description"),
		TEXT("Replace an existing asset at the destination (default: false)"));
	Properties->SetObjectField(TEXT("overwrite"), OverwriteProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("source_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("destination_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AssetImportFile::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse required parameters
	FString SourcePath;
	if (!Arguments->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required argument: source_path"));
	}

	FString DestinationPath;
	if (!Arguments->TryGetStringField(TEXT("destination_path"), DestinationPath) || DestinationPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required argument: destination_path"));
	}

	// Validate source file exists
	if (!FPaths::FileExists(SourcePath))
	{
		return MakeErrorResult(FString::Printf(TEXT("Source file does not exist: %s"), *SourcePath));
	}

	// Parse optional parameters
	FString AssetName;
	if (!Arguments->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
	{
		AssetName = FPaths::GetBaseFilename(SourcePath);
	}

	bool bOverwrite = false;
	Arguments->TryGetBoolField(TEXT("overwrite"), bOverwrite);

	UE_LOG(LogClaireon, Display, TEXT("[MCP] asset.import_file: source=%s dest=%s name=%s overwrite=%d"),
		*SourcePath, *DestinationPath, *AssetName, bOverwrite);

	// Normalize destination path (ensure no trailing slash for the import data)
	FString NormalizedDestPath = DestinationPath;
	if (NormalizedDestPath.EndsWith(TEXT("/")))
	{
		NormalizedDestPath.LeftChopInline(1);
	}

	// Set up automated import
	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	ImportData->Filenames.Add(SourcePath);
	ImportData->DestinationPath = NormalizedDestPath;
	ImportData->bReplaceExisting = bOverwrite;

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<UObject*> ImportedObjects = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);

	if (ImportedObjects.Num() == 0)
	{
		return MakeErrorResult(FString::Printf(TEXT("Import failed. No assets were created from: %s"), *SourcePath));
	}

	UObject* ImportedAsset = ImportedObjects[0];

	// Rename if the user specified a custom asset_name
	FString FinalAssetPath;
	FString AssetType = ImportedAsset->GetClass()->GetName();

	// If a custom name was provided and differs from what was imported, rename
	if (AssetName != ImportedAsset->GetName())
	{
		FString DesiredPath = FString::Printf(TEXT("%s/%s"), *NormalizedDestPath, *AssetName);
		if (!ImportedAsset->Rename(*AssetName, nullptr, REN_DontCreateRedirectors | REN_Test))
		{
			UE_LOG(LogClaireon, Warning, TEXT("[MCP] asset.import_file: Could not rename to %s, keeping original name"), *AssetName);
		}
		else
		{
			ImportedAsset->Rename(*AssetName, nullptr, REN_DontCreateRedirectors);
		}
	}

	// Apply Texture2D-specific settings
	if (UTexture2D* Texture = Cast<UTexture2D>(ImportedAsset))
	{
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->LODGroup = TEXTUREGROUP_UI;
		Texture->SRGB = true;
		Texture->UpdateResource();
		Texture->PostEditChange();
		Texture->GetPackage()->MarkPackageDirty();
	}

	FinalAssetPath = ImportedAsset->GetPathName();

	// Build result
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("success"), true);
	Data->SetStringField(TEXT("asset_path"), FinalAssetPath);
	Data->SetStringField(TEXT("asset_type"), AssetType);
	Data->SetStringField(TEXT("source_file"), SourcePath);

	FString Summary = FString::Printf(TEXT("Imported %s as %s to %s"), *FPaths::GetCleanFilename(SourcePath), *AssetType, *FinalAssetPath);

	return MakeSuccessResult(Data, Summary);
}
