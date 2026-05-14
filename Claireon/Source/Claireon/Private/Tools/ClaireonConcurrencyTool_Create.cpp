// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonConcurrencyTool_Create.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"

#include "Sound/SoundConcurrency.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"

FString FClaireonConcurrencyTool_Create::GetCategory() const { return TEXT("concurrency"); }
FString FClaireonConcurrencyTool_Create::GetOperation() const { return TEXT("create"); }

FString FClaireonConcurrencyTool_Create::GetDescription() const
{
	return TEXT("Create a new USoundConcurrency asset at the given /Game/ path. "
				"Errors if an asset already exists at that path.");
}

TSharedPtr<FJsonObject> FClaireonConcurrencyTool_Create::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Destination path for the new USoundConcurrency (must start with /Game/)"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonConcurrencyTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	const FString Canon = FClaireonSessionManager::CanonicalizePath(AssetPath);
	if (Canon.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid asset path (must start with /Game/)"));
	}
	const FString ObjectName = FPackageName::GetShortName(Canon);

	if (UObject* Existing = LoadObject<UObject>(nullptr, *Canon))
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset already exists at path: %s"), *Canon));
	}

	UPackage* Package = CreatePackage(*Canon);
	UObject* NewAsset = NewObject<UObject>(Package, USoundConcurrency::StaticClass(), *ObjectName,
		RF_Public | RF_Standalone | RF_Transactional | RF_LoadCompleted);
	if (!NewAsset)
	{
		return MakeErrorResult(TEXT("NewObject failed"));
	}
	FAssetRegistryModule::AssetCreated(NewAsset);
	Package->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
	Data->SetStringField(TEXT("kind"), TEXT("concurrency"));
	return MakeSuccessResult(Data, FString::Printf(TEXT("Created Concurrency at %s"), *NewAsset->GetPathName()));
}
