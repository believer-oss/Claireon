// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_Create.h"
#include "ClaireonSessionManager.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#if WITH_GAMEPLAY_CAMERAS

#include "Core/CameraAsset.h"

#define LOCTEXT_NAMESPACE "ClaireonCameraAssetTool_Create"

FString FClaireonCameraAssetTool_Create::GetOperation() const { return TEXT("create"); }

FString FClaireonCameraAssetTool_Create::GetDescription() const
{
	return TEXT("Create an empty UCameraAsset at the given /Game/ path. Errors if an asset already exists.");
}

TSharedPtr<FJsonObject> FClaireonCameraAssetTool_Create::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Destination path for the new UCameraAsset (must start with /Game/)"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonCameraAssetTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FScopedTransaction Transaction(LOCTEXT("CreateCameraAsset", "[Claireon] Create Camera Asset"));

	UPackage* Package = CreatePackage(*Canon);
	if (!Package)
	{
		return MakeErrorResult(TEXT("CreatePackage failed"));
	}
	UCameraAsset* NewAsset = NewObject<UCameraAsset>(Package, UCameraAsset::StaticClass(), *ObjectName,
		RF_Public | RF_Standalone | RF_Transactional | RF_LoadCompleted);
	if (!NewAsset)
	{
		return MakeErrorResult(TEXT("NewObject failed"));
	}
	FAssetRegistryModule::AssetCreated(NewAsset);
	Package->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
	Data->SetStringField(TEXT("kind"), TEXT("camera_asset"));
	return MakeSuccessResult(Data, FString::Printf(TEXT("Created CameraAsset at %s"), *NewAsset->GetPathName()));
}

#undef LOCTEXT_NAMESPACE

#endif // WITH_GAMEPLAY_CAMERAS
