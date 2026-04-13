// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimTools.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimComposite.h"
#include "Animation/Skeleton.h"
#include "Factories/AnimMontageFactory.h"
#include "Factories/AnimCompositeFactory.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// File-scope helpers
// ============================================================================

namespace
{
	bool ValidateNewAssetPath(const FString& InPath, FString& OutCanonPath, FString& OutAssetName, FString& OutError)
	{
		OutCanonPath = FClaireonSessionManager::CanonicalizePath(InPath);
		if (OutCanonPath.IsEmpty())
		{
			OutError = TEXT("Invalid asset path. Must start with /Game/.");
			return false;
		}
		if (StaticFindObject(nullptr, nullptr, *OutCanonPath))
		{
			OutError = FString::Printf(TEXT("Asset already exists at '%s'"), *OutCanonPath);
			return false;
		}
		OutAssetName = FPackageName::GetShortName(OutCanonPath);
		return true;
	}

	bool SaveNewAsset(UObject* Asset, FString& OutError)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();
		FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		FSavePackageResultStruct Result = UPackage::Save(Package, Asset, *PackageFileName, SaveArgs);
		if (Result.Result != ESavePackageResult::Success)
		{
			OutError = FString::Printf(TEXT("Failed to save package '%s'"), *Package->GetName());
			return false;
		}
		return true;
	}
}

// ============================================================================
// claireon.anim_create_montage
// ============================================================================

FString ClaireonAnimTool_CreateMontage::GetName() const { return TEXT("claireon.anim_create_montage"); }

FString ClaireonAnimTool_CreateMontage::GetDescription() const
{
	return TEXT("Create a new AnimMontage asset with an optional source animation and slot name.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_CreateMontage::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("path"), TEXT("Target asset path (e.g. /Game/Char/STELLA/Anim/AM_NewMontage)"), true);
	S.AddString(TEXT("skeleton"), TEXT("Skeleton asset path (e.g. /Game/Char/STELLA/STELLA_Skeleton)"), true);
	S.AddString(TEXT("animation"), TEXT("Source AnimSequence asset path to populate the montage"));
	S.AddString(TEXT("slot_name"), TEXT("Montage slot name (default: DefaultSlot)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_CreateMontage::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Path;
	if (!Arguments->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: path"));
	}
	FString SkeletonPath;
	if (!Arguments->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: skeleton"));
	}

	// Validate target path
	FString CanonPath, AssetName, Error;
	if (!ValidateNewAssetPath(Path, CanonPath, AssetName, Error))
	{
		return MakeErrorResult(Error);
	}

	// Load skeleton
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
	if (!Skeleton)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load skeleton at '%s'"), *SkeletonPath));
	}

	// Optionally load source animation
	FString AnimPath;
	UAnimSequence* SourceAnim = nullptr;
	if (Arguments->TryGetStringField(TEXT("animation"), AnimPath) && !AnimPath.IsEmpty())
	{
		SourceAnim = LoadObject<UAnimSequence>(nullptr, *AnimPath);
		if (!SourceAnim)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to load animation at '%s'"), *AnimPath));
		}
		// Validate skeleton compatibility
		if (SourceAnim->GetSkeleton() != Skeleton)
		{
			return MakeErrorResult(TEXT("Source animation skeleton does not match the provided skeleton"));
		}
	}

	// Optional slot name
	FString SlotName = TEXT("DefaultSlot");
	Arguments->TryGetStringField(TEXT("slot_name"), SlotName);

	// Create montage via engine factory (handles frame rate, default section, etc.)
	UPackage* Package = CreatePackage(*CanonPath);

	UAnimMontageFactory* Factory = NewObject<UAnimMontageFactory>();
	Factory->TargetSkeleton = Skeleton;
	if (SourceAnim)
	{
		Factory->SourceAnimation = SourceAnim;
	}

	UAnimMontage* Montage = Cast<UAnimMontage>(
		Factory->FactoryCreateNew(UAnimMontage::StaticClass(), Package,
			FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
	if (!Montage)
	{
		return MakeErrorResult(TEXT("Factory failed to create montage"));
	}

	// Set custom slot name if specified
	if (SlotName != TEXT("DefaultSlot") && Montage->SlotAnimTracks.Num() > 0)
	{
		Montage->SlotAnimTracks[0].SlotName = FName(*SlotName);
	}

	// Register and save
	FAssetRegistryModule::AssetCreated(Montage);

	FString SaveError;
	if (!SaveNewAsset(Montage, SaveError))
	{
		return MakeErrorResult(SaveError);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Montage->GetPathName());
	Result->SetStringField(TEXT("asset_type"), TEXT("AnimMontage"));
	Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Result->SetStringField(TEXT("slot_name"), SlotName);
	if (SourceAnim)
	{
		Result->SetStringField(TEXT("animation"), SourceAnim->GetPathName());
		Result->SetNumberField(TEXT("length"), Montage->GetPlayLength());
	}

	return MakeSuccessResult(Result, FString::Printf(TEXT("Created montage '%s'"), *AssetName));
}

// ============================================================================
// claireon.anim_create_composite
// ============================================================================

FString ClaireonAnimTool_CreateComposite::GetName() const { return TEXT("claireon.anim_create_composite"); }

FString ClaireonAnimTool_CreateComposite::GetDescription() const
{
	return TEXT("Create a new AnimComposite asset with an optional source animation.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_CreateComposite::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("path"), TEXT("Target asset path (e.g. /Game/Char/STELLA/Anim/AC_NewComposite)"), true);
	S.AddString(TEXT("skeleton"), TEXT("Skeleton asset path (e.g. /Game/Char/STELLA/STELLA_Skeleton)"), true);
	S.AddString(TEXT("animation"), TEXT("Source AnimSequence asset path to populate the composite"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_CreateComposite::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Path;
	if (!Arguments->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: path"));
	}
	FString SkeletonPath;
	if (!Arguments->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: skeleton"));
	}

	// Validate target path
	FString CanonPath, AssetName, Error;
	if (!ValidateNewAssetPath(Path, CanonPath, AssetName, Error))
	{
		return MakeErrorResult(Error);
	}

	// Load skeleton
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
	if (!Skeleton)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load skeleton at '%s'"), *SkeletonPath));
	}

	// Optionally load source animation
	FString AnimPath;
	UAnimSequence* SourceAnim = nullptr;
	if (Arguments->TryGetStringField(TEXT("animation"), AnimPath) && !AnimPath.IsEmpty())
	{
		SourceAnim = LoadObject<UAnimSequence>(nullptr, *AnimPath);
		if (!SourceAnim)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to load animation at '%s'"), *AnimPath));
		}
		if (SourceAnim->GetSkeleton() != Skeleton)
		{
			return MakeErrorResult(TEXT("Source animation skeleton does not match the provided skeleton"));
		}
	}

	// Create composite via engine factory (handles frame rate, etc.)
	UPackage* Package = CreatePackage(*CanonPath);

	UAnimCompositeFactory* CompositeFactory = NewObject<UAnimCompositeFactory>();
	CompositeFactory->TargetSkeleton = Skeleton;
	if (SourceAnim)
	{
		CompositeFactory->SourceAnimation = SourceAnim;
	}

	UAnimComposite* Composite = Cast<UAnimComposite>(
		CompositeFactory->FactoryCreateNew(UAnimComposite::StaticClass(), Package,
			FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
	if (!Composite)
	{
		return MakeErrorResult(TEXT("Factory failed to create composite"));
	}

	// Composite factory doesn't call UpdateCommonTargetFrameRate (unlike montage factory).
	// Set CommonTargetFrameRate via reflection since it's a protected UPROPERTY.
	if (SourceAnim)
	{
		FProperty* FrameRateProp = UAnimCompositeBase::StaticClass()->FindPropertyByName(TEXT("CommonTargetFrameRate"));
		if (FrameRateProp)
		{
			FFrameRate* TargetRate = FrameRateProp->ContainerPtrToValuePtr<FFrameRate>(Composite);
			*TargetRate = SourceAnim->GetSamplingFrameRate();
		}
	}

	// Register and save
	FAssetRegistryModule::AssetCreated(Composite);

	FString SaveError;
	if (!SaveNewAsset(Composite, SaveError))
	{
		return MakeErrorResult(SaveError);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), Composite->GetPathName());
	Result->SetStringField(TEXT("asset_type"), TEXT("AnimComposite"));
	Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	if (SourceAnim)
	{
		Result->SetStringField(TEXT("animation"), SourceAnim->GetPathName());
		Result->SetNumberField(TEXT("length"), Composite->GetPlayLength());
	}

	return MakeSuccessResult(Result, FString::Printf(TEXT("Created composite '%s'"), *AssetName));
}

// ============================================================================
// claireon.anim_duplicate_asset
// ============================================================================

FString ClaireonAnimTool_DuplicateAsset::GetName() const { return TEXT("claireon.anim_duplicate_asset"); }

FString ClaireonAnimTool_DuplicateAsset::GetDescription() const
{
	return TEXT("Duplicate an existing animation asset (AnimSequence, AnimMontage, or AnimComposite) to a new path.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_DuplicateAsset::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("source_path"), TEXT("Path of the animation asset to duplicate"), true);
	S.AddString(TEXT("dest_path"), TEXT("Destination path for the duplicated asset"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_DuplicateAsset::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SourcePath;
	if (!Arguments->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: source_path"));
	}
	FString DestPath;
	if (!Arguments->TryGetStringField(TEXT("dest_path"), DestPath) || DestPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: dest_path"));
	}

	// Canonicalize paths
	SourcePath = FClaireonSessionManager::CanonicalizePath(SourcePath);
	if (SourcePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid source_path. Must start with /Game/."));
	}
	DestPath = FClaireonSessionManager::CanonicalizePath(DestPath);
	if (DestPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid dest_path. Must start with /Game/."));
	}

	// Load source asset
	UObject* SourceObj = FSoftObjectPath(SourcePath).TryLoad();
	if (!SourceObj)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load source asset at '%s'"), *SourcePath));
	}

	// Verify it's an animation asset
	UAnimSequenceBase* SourceAnim = Cast<UAnimSequenceBase>(SourceObj);
	if (!SourceAnim)
	{
		return MakeErrorResult(FString::Printf(TEXT("Source asset '%s' is not an animation asset (AnimSequence, AnimMontage, or AnimComposite)"), *SourcePath));
	}

	// Check dest doesn't already exist
	if (StaticFindObject(nullptr, nullptr, *DestPath))
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset already exists at '%s'"), *DestPath));
	}

	// Parse dest into folder + name
	FString DestFolder = FPackageName::GetLongPackagePath(DestPath);
	FString DestName = FPackageName::GetShortName(DestPath);

	// Duplicate via engine AssetTools
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* NewAsset = AssetTools.DuplicateAsset(DestName, DestFolder, SourceObj);
	if (!NewAsset)
	{
		return MakeErrorResult(FString::Printf(TEXT("Engine failed to duplicate asset to '%s/%s'"), *DestFolder, *DestName));
	}

	// Save the new package
	FString SaveError;
	if (!SaveNewAsset(NewAsset, SaveError))
	{
		return MakeErrorResult(SaveError);
	}

	// Detect type
	FString AssetType = TEXT("AnimSequence");
	if (Cast<UAnimMontage>(NewAsset))
		AssetType = TEXT("AnimMontage");
	else if (Cast<UAnimComposite>(NewAsset))
		AssetType = TEXT("AnimComposite");

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_path"), SourceAnim->GetPathName());
	Result->SetStringField(TEXT("dest_path"), NewAsset->GetPathName());
	Result->SetStringField(TEXT("asset_type"), AssetType);

	return MakeSuccessResult(Result, FString::Printf(TEXT("Duplicated %s to '%s'"), *AssetType, *DestName));
}
