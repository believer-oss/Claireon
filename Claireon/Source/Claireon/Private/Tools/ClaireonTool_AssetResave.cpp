// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetResave.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

FString ClaireonTool_AssetResave::GetName() const
{
	return TEXT("claireon.asset_resave");
}

FString ClaireonTool_AssetResave::GetDescription() const
{
	return TEXT("Resave specified assets to update serialization");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetResave::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// assetPaths (required)
	TSharedPtr<FJsonObject> AssetPathsProp = MakeShared<FJsonObject>();
	AssetPathsProp->SetStringField(TEXT("type"), TEXT("array"));
	TSharedPtr<FJsonObject> AssetPathsItems = MakeShared<FJsonObject>();
	AssetPathsItems->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathsProp->SetObjectField(TEXT("items"), AssetPathsItems);
	AssetPathsProp->SetStringField(TEXT("description"),
		TEXT("Array of Unreal asset paths to resave (e.g. /Game/Characters/BP_Player)"));
	Properties->SetObjectField(TEXT("assetPaths"), AssetPathsProp);

	// force (optional)
	TSharedPtr<FJsonObject> ForceProp = MakeShared<FJsonObject>();
	ForceProp->SetStringField(TEXT("type"), TEXT("boolean"));
	ForceProp->SetStringField(TEXT("description"),
		TEXT("Force resave even if the asset is not dirty (default: false)"));
	Properties->SetObjectField(TEXT("force"), ForceProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("assetPaths")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AssetResave::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	const TArray<TSharedPtr<FJsonValue>>* AssetPathsArray = nullptr;
	if (!Arguments->TryGetArrayField(TEXT("assetPaths"), AssetPathsArray) || !AssetPathsArray)
	{
		return MakeErrorResult(TEXT("Missing required argument: assetPaths"));
	}

	bool bForce = false;
	Arguments->TryGetBoolField(TEXT("force"), bForce);

	TArray<TSharedPtr<FJsonValue>> ResavedArray;
	TArray<TSharedPtr<FJsonValue>> FailedArray;

	for (const TSharedPtr<FJsonValue>& PathValue : *AssetPathsArray)
	{
		FString AssetPath = PathValue->AsString();
		if (AssetPath.IsEmpty())
		{
			continue;
		}

		// Resolve path
		auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
		if (!ResolveResult.bSuccess)
		{
			FailedArray.Add(MakeShared<FJsonValueString>(AssetPath));
			UE_LOG(LogTemp, Warning, TEXT("ClaireonTool_AssetResave: Invalid path: %s"), *ResolveResult.Error);
			continue;
		}
		FString PackagePathStr = ResolveResult.ResolvedPath.Path;
		// Post-resolver: strip sub-object reference for LoadPackage
		int32 DotIndex;
		if (PackagePathStr.FindChar(TEXT('.'), DotIndex))
		{
			PackagePathStr = PackagePathStr.Left(DotIndex);
		}

		// Load the package
		UPackage* Package = LoadPackage(nullptr, *PackagePathStr, LOAD_None);
		if (!Package)
		{
			FailedArray.Add(MakeShared<FJsonValueString>(AssetPath));
			UE_LOG(LogTemp, Warning, TEXT("ClaireonTool_AssetResave: Failed to load package: %s"), *PackagePathStr);
			continue;
		}

		// Skip if not dirty and not forcing
		if (!bForce && !Package->IsDirty())
		{
			// Mark dirty so SavePackage will actually write
			Package->MarkPackageDirty();
		}

		// Build the filename on disk
		FString PackageFilename;
		if (!FPackageName::TryConvertLongPackageNameToFilename(PackagePathStr, PackageFilename, FPackageName::GetAssetPackageExtension()))
		{
			FailedArray.Add(MakeShared<FJsonValueString>(AssetPath));
			UE_LOG(LogTemp, Warning, TEXT("ClaireonTool_AssetResave: Could not resolve filename for: %s"), *PackagePathStr);
			continue;
		}

		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GError;

		if (ClaireonSafeExec::DidLastExecutionCrash())
		{
			FailedArray.Add(MakeShared<FJsonValueString>(AssetPath));
			UE_LOG(LogTemp, Warning, TEXT("ClaireonTool_AssetResave: Save blocked after crash for: %s"), *PackagePathStr);
			continue;
		}
		const FSavePackageResultStruct SaveResult = UPackage::Save(Package, nullptr, *PackageFilename, SaveArgs);
		if (SaveResult.Result == ESavePackageResult::Success)
		{
			ResavedArray.Add(MakeShared<FJsonValueString>(AssetPath));
		}
		else
		{
			FailedArray.Add(MakeShared<FJsonValueString>(AssetPath));
			UE_LOG(LogTemp, Warning, TEXT("ClaireonTool_AssetResave: Save failed for: %s"), *PackagePathStr);
		}
	}

	int32 ResavedCount = ResavedArray.Num();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("resaved"), ResavedArray);
	Data->SetArrayField(TEXT("failed"), FailedArray);
	Data->SetNumberField(TEXT("count"), ResavedCount);

	FString Summary = FString::Printf(TEXT("Resaved %d asset%s successfully"),
		ResavedCount, ResavedCount == 1 ? TEXT("") : TEXT("s"));
	if (FailedArray.Num() > 0)
	{
		Summary += FString::Printf(TEXT(", %d failed"), FailedArray.Num());
	}

	return MakeSuccessResult(Data, Summary);
}
