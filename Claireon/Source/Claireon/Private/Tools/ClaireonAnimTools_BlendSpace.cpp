// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimTools_BlendSpace.h"
#include "Tools/ClaireonAnimHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonNameResolver.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"
#include "Animation/BlendSpace.h"
#include "Animation/BlendSpace1D.h"
#include "Animation/AimOffsetBlendSpace.h"
#include "Animation/AimOffsetBlendSpace1D.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMetaData.h"
#include "Animation/Skeleton.h"
#include "Factories/BlendSpaceFactoryNew.h"
#include "Factories/BlendSpaceFactory1D.h"
#include "Factories/AimOffsetBlendSpaceFactoryNew.h"
#include "Factories/AimOffsetBlendSpaceFactory1D.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ============================================================================
// File-scope helpers
// ============================================================================

namespace
{
	bool ValidateNewBlendSpacePath(const FString& InPath, FString& OutCanonPath, FString& OutAssetName, FString& OutError)
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

	bool SaveBlendSpaceAsset(UObject* Asset, FString& OutError)
	{
		UPackage* Package = Asset->GetOutermost();
		Package->FullyLoad();
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

	UBlendSpace* LoadBlendSpace(const FString& AssetPath, FString& OutError)
	{
		auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
		if (!ResolveResult.bSuccess)
		{
			OutError = ResolveResult.Error;
			return nullptr;
		}

		FSoftObjectPath SoftPath(ResolveResult.ResolvedPath.Path);
		UObject* LoadedObj = SoftPath.TryLoad();
		if (!LoadedObj)
		{
			OutError = FString::Printf(TEXT("Failed to load asset at '%s'"), *ResolveResult.ResolvedPath.Path);
			return nullptr;
		}

		UBlendSpace* BlendSpace = Cast<UBlendSpace>(LoadedObj);
		if (!BlendSpace)
		{
			OutError = FString::Printf(TEXT("Asset '%s' is not a BlendSpace (got %s)"), *ResolveResult.ResolvedPath.Path, *LoadedObj->GetClass()->GetName());
			return nullptr;
		}

		return BlendSpace;
	}

	FString DetectBlendSpaceType(const UBlendSpace* BlendSpace)
	{
		// Check most-specific first
		if (Cast<UAimOffsetBlendSpace1D>(BlendSpace))
		{
			return TEXT("AimOffset1D");
		}
		if (Cast<UAimOffsetBlendSpace>(BlendSpace))
		{
			return TEXT("AimOffset");
		}
		if (Cast<UBlendSpace1D>(BlendSpace))
		{
			return TEXT("BlendSpace1D");
		}
		return TEXT("BlendSpace");
	}

	bool Is1DBlendSpace(const UBlendSpace* BlendSpace)
	{
		return Cast<const UBlendSpace1D>(BlendSpace) != nullptr;
	}

	FString FormatSmoothingType(EFilterInterpolationType Type)
	{
		switch (Type)
		{
		case EFilterInterpolationType::BSIT_Average:           return TEXT("Average");
		case EFilterInterpolationType::BSIT_Linear:            return TEXT("Linear");
		case EFilterInterpolationType::BSIT_Cubic:             return TEXT("Cubic");
		case EFilterInterpolationType::BSIT_EaseInOut:         return TEXT("EaseInOut");
		case EFilterInterpolationType::BSIT_SpringDamper:      return TEXT("SpringDamper");
		case EFilterInterpolationType::BSIT_ExponentialDecay:  return TEXT("Exponential");
		default:                                               return TEXT("Unknown");
		}
	}

	FString FormatBlendParameter(const FBlendParameter& Param, const FInterpolationParameter& Interp, int32 Index)
	{
		FString Result = FString::Printf(TEXT("  Axis %d: \"%s\"\n"), Index, *Param.DisplayName);
		Result += FString::Printf(TEXT("    Range: [%.2f .. %.2f]\n"), Param.Min, Param.Max);
		Result += FString::Printf(TEXT("    Grid Divisions: %d\n"), Param.GridNum);
		Result += FString::Printf(TEXT("    Snap to Grid: %s\n"), Param.bSnapToGrid ? TEXT("Yes") : TEXT("No"));
		Result += FString::Printf(TEXT("    Wrap Input: %s\n"), Param.bWrapInput ? TEXT("Yes") : TEXT("No"));
		Result += FString::Printf(TEXT("    Smoothing Type: %s\n"), *FormatSmoothingType(Interp.InterpolationType));
		if (Interp.InterpolationTime > 0.f)
		{
			Result += FString::Printf(TEXT("    Smoothing Time: %.3f\n"), Interp.InterpolationTime);
			if (Interp.InterpolationType == EFilterInterpolationType::BSIT_SpringDamper)
			{
				Result += FString::Printf(TEXT("    Damping Ratio: %.3f\n"), Interp.DampingRatio);
			}
			if (Interp.MaxSpeed > 0.f)
			{
				Result += FString::Printf(TEXT("    Max Speed: %.2f\n"), Interp.MaxSpeed);
			}
		}
		else
		{
			Result += TEXT("    Smoothing: Off\n");
		}
		return Result;
	}

	FString FormatNotifyTriggerMode(ENotifyTriggerMode::Type Mode)
	{
		switch (Mode)
		{
		case ENotifyTriggerMode::AllAnimations:             return TEXT("AllAnimations");
		case ENotifyTriggerMode::HighestWeightedAnimation:  return TEXT("HighestWeightedAnimation");
		case ENotifyTriggerMode::None:                      return TEXT("None");
		default:                                            return TEXT("Unknown");
		}
	}

	FString FormatBlendSpaceInspection(const UBlendSpace* BlendSpace, bool bFullDetail)
	{
		FString TypeStr = DetectBlendSpaceType(BlendSpace);
		bool bIs1D = Is1DBlendSpace(BlendSpace);

		FString Result;
		Result += FString::Printf(TEXT("=== %s: %s ===\n"), *TypeStr, *BlendSpace->GetName());
		Result += FString::Printf(TEXT("Path: %s\n"), *BlendSpace->GetPathName());

		USkeleton* Skeleton = BlendSpace->GetSkeleton();
		Result += FString::Printf(TEXT("Skeleton: %s\n"), Skeleton ? *Skeleton->GetPathName() : TEXT("(none)"));

		// Axes (with per-axis interpolation inline)
		Result += TEXT("\n--- Axes ---\n");
		const FBlendParameter& Param0 = BlendSpace->GetBlendParameter(0);
		Result += FormatBlendParameter(Param0, BlendSpace->InterpolationParam[0], 0);
		if (!bIs1D)
		{
			const FBlendParameter& Param1 = BlendSpace->GetBlendParameter(1);
			Result += FormatBlendParameter(Param1, BlendSpace->InterpolationParam[1], 1);
		}

		// Samples
		const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
		Result += FString::Printf(TEXT("\n--- Samples (%d) ---\n"), Samples.Num());
		for (int32 i = 0; i < Samples.Num(); ++i)
		{
			const FBlendSample& Sample = Samples[i];
			FString AnimName = Sample.Animation ? Sample.Animation->GetName() : TEXT("(none)");
			if (bIs1D)
			{
				Result += FString::Printf(TEXT("  [%d] %s  X=%.2f  RateScale=%.2f\n"),
					i, *AnimName, Sample.SampleValue.X, Sample.RateScale);
			}
			else
			{
				Result += FString::Printf(TEXT("  [%d] %s  X=%.2f  Y=%.2f  RateScale=%.2f\n"),
					i, *AnimName, Sample.SampleValue.X, Sample.SampleValue.Y, Sample.RateScale);
			}
			if (bFullDetail && Sample.Animation)
			{
				Result += FString::Printf(TEXT("       Path: %s\n"), *Sample.Animation->GetPathName());
			}
		}

		// Sample Smoothing
		Result += TEXT("\n--- Sample Smoothing ---\n");
		Result += FString::Printf(TEXT("  Weight Speed: %.2f\n"), BlendSpace->TargetWeightInterpolationSpeedPerSec);
		Result += FString::Printf(TEXT("  Easing: %s\n"), BlendSpace->bTargetWeightInterpolationEaseInOut ? TEXT("Yes") : TEXT("No"));
		Result += FString::Printf(TEXT("  Allow Mesh Space Blending: %s\n"), BlendSpace->bAllowMeshSpaceBlending ? TEXT("Yes") : TEXT("No"));

		// Settings
		Result += TEXT("\n--- Settings ---\n");
		Result += FString::Printf(TEXT("  Notify Trigger Mode: %s\n"), *FormatNotifyTriggerMode(BlendSpace->NotifyTriggerMode));
		Result += FString::Printf(TEXT("  Loop: %s\n"), BlendSpace->bLoop ? TEXT("Yes") : TEXT("No"));
		Result += FString::Printf(TEXT("  Allow Marker Sync: %s\n"), BlendSpace->bAllowMarkerBasedSync ? TEXT("Yes") : TEXT("No"));
		Result += FString::Printf(TEXT("  Use Grid Interpolation: %s\n"), BlendSpace->bInterpolateUsingGrid ? TEXT("Yes") : TEXT("No"));

		// Preferred Triangulation Direction
		{
			FString TriDir;
			switch (BlendSpace->PreferredTriangulationDirection)
			{
			case EPreferredTriangulationDirection::None:        TriDir = TEXT("None"); break;
			case EPreferredTriangulationDirection::Tangential:  TriDir = TEXT("Tangential"); break;
			case EPreferredTriangulationDirection::Radial:      TriDir = TEXT("Radial"); break;
			default:                                            TriDir = TEXT("Unknown"); break;
			}
			Result += FString::Printf(TEXT("  Preferred Triangulation: %s\n"), *TriDir);
		}

		// AxisToScaleAnimation via reflection
		{
			FProperty* ScaleAxisProp = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("AxisToScaleAnimation"));
			if (ScaleAxisProp)
			{
				TEnumAsByte<EBlendSpaceAxis>* AxisPtr = ScaleAxisProp->ContainerPtrToValuePtr<TEnumAsByte<EBlendSpaceAxis>>(const_cast<UBlendSpace*>(BlendSpace));
				if (AxisPtr)
				{
					FString AxisStr;
					switch (AxisPtr->GetValue())
					{
					case BSA_None: AxisStr = TEXT("None"); break;
					case BSA_X:    AxisStr = TEXT("X"); break;
					case BSA_Y:    AxisStr = TEXT("Y"); break;
					default:       AxisStr = TEXT("Unknown"); break;
					}
					Result += FString::Printf(TEXT("  Axis to Scale Animation: %s\n"), *AxisStr);
				}
			}
		}

		// BlendSpace1D-specific
		{
			const UBlendSpace1D* BS1D = Cast<const UBlendSpace1D>(BlendSpace);
			if (BS1D)
			{
				Result += FString::Printf(TEXT("  Scale Animation: %s\n"), BS1D->bScaleAnimation ? TEXT("Yes") : TEXT("No"));
			}
		}

		// Metadata
		FString MetadataText = ClaireonAnimHelpers::FormatMetadata(BlendSpace, bFullDetail);
		if (!MetadataText.IsEmpty())
		{
			Result += TEXT("\n--- Metadata ---\n");
			Result += MetadataText;
		}

		return Result;
	}

	bool ValidateAxisIndex(const UBlendSpace* BlendSpace, int32 AxisIndex, FString& OutError)
	{
		bool bIs1D = Is1DBlendSpace(BlendSpace);
		int32 MaxAxis = bIs1D ? 0 : 1;
		if (AxisIndex < 0 || AxisIndex > MaxAxis)
		{
			OutError = FString::Printf(TEXT("Axis index %d out of range [0, %d] for %s"),
				AxisIndex, MaxAxis, bIs1D ? TEXT("1D blend space") : TEXT("2D blend space"));
			return false;
		}
		return true;
	}

	FBlendParameter* GetBlendParameterMutable(UBlendSpace* BlendSpace, int32 Index)
	{
		FProperty* Prop = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("BlendParameters"));
		if (!Prop) return nullptr;
		FBlendParameter* Params = Prop->ContainerPtrToValuePtr<FBlendParameter>(BlendSpace);
		if (!Params) return nullptr;
		return &Params[Index];
	}

	EFilterInterpolationType ParseSmoothingType(const FString& TypeStr, bool& bValid)
	{
		bValid = true;
		if (TypeStr == TEXT("Average"))      return EFilterInterpolationType::BSIT_Average;
		if (TypeStr == TEXT("Linear"))       return EFilterInterpolationType::BSIT_Linear;
		if (TypeStr == TEXT("Cubic"))        return EFilterInterpolationType::BSIT_Cubic;
		if (TypeStr == TEXT("EaseInOut"))    return EFilterInterpolationType::BSIT_EaseInOut;
		if (TypeStr == TEXT("SpringDamper")) return EFilterInterpolationType::BSIT_SpringDamper;
		if (TypeStr == TEXT("Exponential"))  return EFilterInterpolationType::BSIT_ExponentialDecay;
		bValid = false;
		return EFilterInterpolationType::BSIT_Average;
	}
}

// ============================================================================
// claireon.blendspace_create
// ============================================================================

FString ClaireonAnimTool_BlendSpaceCreate::GetName() const { return TEXT("claireon.blendspace_create"); }

FString ClaireonAnimTool_BlendSpaceCreate::GetDescription() const
{
	return TEXT("Create a new BlendSpace, BlendSpace1D, AimOffset, or AimOffset1D asset on disk. Stateless / non-session: writes the asset and saves it immediately, no open session required. Requires target path, skeleton, and asset type. Optionally seed notify trigger mode and metadata at creation time.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_BlendSpaceCreate::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("path"), TEXT("Target asset path (e.g. /Game/Char/STELLA/Anim/BS_Locomotion)"), true);
	S.AddString(TEXT("skeleton"), TEXT("Skeleton asset path (e.g. /Game/Char/STELLA/STELLA_Skeleton)"), true);
	S.AddEnum(TEXT("type"), TEXT("Type of blend space to create"),
		{TEXT("BlendSpace"), TEXT("BlendSpace1D"), TEXT("AimOffset"), TEXT("AimOffset1D")}, true);
	S.AddEnum(TEXT("notify_trigger_mode"), TEXT("Notify trigger mode (default: AllAnimations)"),
		{TEXT("AllAnimations"), TEXT("HighestWeightedAnimation"), TEXT("None")});
	S.AddString(TEXT("metadata"), TEXT("Comma-separated list of metadata class names to instantiate on creation"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_BlendSpaceCreate::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse required params
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
	FString TypeStr;
	if (!Arguments->TryGetStringField(TEXT("type"), TypeStr) || TypeStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: type"));
	}

	// Validate target path
	FString CanonPath, AssetName, Error;
	if (!ValidateNewBlendSpacePath(Path, CanonPath, AssetName, Error))
	{
		return MakeErrorResult(Error);
	}

	// Load skeleton
	USkeleton* Skeleton = LoadObject<USkeleton>(nullptr, *SkeletonPath);
	if (!Skeleton)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load skeleton at '%s'"), *SkeletonPath));
	}

	// Create package
	UPackage* Package = CreatePackage(*CanonPath);

	// Create via appropriate factory
	UBlendSpace* NewBlendSpace = nullptr;

	if (TypeStr == TEXT("BlendSpace"))
	{
		UBlendSpaceFactoryNew* Factory = NewObject<UBlendSpaceFactoryNew>();
		Factory->TargetSkeleton = Skeleton;
		NewBlendSpace = Cast<UBlendSpace>(
			Factory->FactoryCreateNew(UBlendSpace::StaticClass(), Package,
				FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
	}
	else if (TypeStr == TEXT("BlendSpace1D"))
	{
		UBlendSpaceFactory1D* Factory = NewObject<UBlendSpaceFactory1D>();
		Factory->TargetSkeleton = Skeleton;
		NewBlendSpace = Cast<UBlendSpace>(
			Factory->FactoryCreateNew(UBlendSpace1D::StaticClass(), Package,
				FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
	}
	else if (TypeStr == TEXT("AimOffset"))
	{
		UAimOffsetBlendSpaceFactoryNew* Factory = NewObject<UAimOffsetBlendSpaceFactoryNew>();
		Factory->TargetSkeleton = Skeleton;
		NewBlendSpace = Cast<UBlendSpace>(
			Factory->FactoryCreateNew(UAimOffsetBlendSpace::StaticClass(), Package,
				FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
	}
	else if (TypeStr == TEXT("AimOffset1D"))
	{
		UAimOffsetBlendSpaceFactory1D* Factory = NewObject<UAimOffsetBlendSpaceFactory1D>();
		Factory->TargetSkeleton = Skeleton;
		NewBlendSpace = Cast<UBlendSpace>(
			Factory->FactoryCreateNew(UAimOffsetBlendSpace1D::StaticClass(), Package,
				FName(*AssetName), RF_Public | RF_Standalone, nullptr, GWarn));
	}
	else
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid type '%s'. Must be BlendSpace, BlendSpace1D, AimOffset, or AimOffset1D"), *TypeStr));
	}

	if (!NewBlendSpace)
	{
		return MakeErrorResult(TEXT("Factory failed to create blend space"));
	}

	// Optional: set notify trigger mode
	FString NotifyMode;
	if (Arguments->TryGetStringField(TEXT("notify_trigger_mode"), NotifyMode) && !NotifyMode.IsEmpty())
	{
		if (NotifyMode == TEXT("AllAnimations"))
		{
			NewBlendSpace->NotifyTriggerMode = ENotifyTriggerMode::AllAnimations;
		}
		else if (NotifyMode == TEXT("HighestWeightedAnimation"))
		{
			NewBlendSpace->NotifyTriggerMode = ENotifyTriggerMode::HighestWeightedAnimation;
		}
		else if (NotifyMode == TEXT("None"))
		{
			NewBlendSpace->NotifyTriggerMode = ENotifyTriggerMode::None;
		}
	}

	// Optional: add metadata
	FString MetadataStr;
	if (Arguments->TryGetStringField(TEXT("metadata"), MetadataStr) && !MetadataStr.IsEmpty())
	{
		TArray<FString> ClassNames;
		MetadataStr.ParseIntoArray(ClassNames, TEXT(","), true);
		for (const FString& RawName : ClassNames)
		{
			FString ClassName = RawName.TrimStartAndEnd();
			if (ClassName.IsEmpty()) continue;

			ClaireonNameResolver::FNameResolveResult NameResult;
			UClass* MetaDataClass = ClaireonNameResolver::ResolveClassName(ClassName, UAnimMetaData::StaticClass(), NameResult);
			if (!MetaDataClass)
			{
				// Non-fatal: warn but continue
				UE_LOG(LogClaireon, Warning, TEXT("BlendSpace create: could not resolve metadata class '%s': %s"), *ClassName, *NameResult.Error);
				continue;
			}

			UAnimMetaData* NewMetaData = NewObject<UAnimMetaData>(NewBlendSpace, MetaDataClass);
			if (NewMetaData)
			{
				NewBlendSpace->AddMetaData(NewMetaData);
			}
		}
	}

	// Register and save
	FAssetRegistryModule::AssetCreated(NewBlendSpace);

	FString SaveError;
	if (!SaveBlendSpaceAsset(NewBlendSpace, SaveError))
	{
		return MakeErrorResult(SaveError);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("path"), NewBlendSpace->GetPathName());
	Result->SetStringField(TEXT("asset_type"), TypeStr);
	Result->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Result->SetStringField(TEXT("notify_trigger_mode"), FormatNotifyTriggerMode(NewBlendSpace->NotifyTriggerMode));

	int32 MetaCount = NewBlendSpace->GetMetaData().Num();
	if (MetaCount > 0)
	{
		Result->SetNumberField(TEXT("metadata_count"), MetaCount);
	}

	return MakeSuccessResult(Result, FString::Printf(TEXT("Created %s '%s'"), *TypeStr, *AssetName));
}

// ============================================================================
// claireon.blendspace_duplicate
// ============================================================================

FString ClaireonAnimTool_BlendSpaceDuplicate::GetName() const { return TEXT("claireon.blendspace_duplicate"); }

FString ClaireonAnimTool_BlendSpaceDuplicate::GetDescription() const
{
	return TEXT("Duplicate an existing BlendSpace or AimOffset asset to a new path. Stateless / non-session: writes the duplicate immediately, no open session required. Auto-detects the source asset's specific type. Common pitfall: target package directory must already exist before invoking.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_BlendSpaceDuplicate::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("source_path"), TEXT("Path of the blend space asset to duplicate"), true);
	S.AddString(TEXT("dest_path"), TEXT("Destination path for the duplicated asset"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_BlendSpaceDuplicate::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	// Load source
	FString LoadError;
	UBlendSpace* SourceBS = LoadBlendSpace(SourcePath, LoadError);
	if (!SourceBS)
	{
		return MakeErrorResult(LoadError);
	}

	// Canonicalize dest path
	DestPath = FClaireonSessionManager::CanonicalizePath(DestPath);
	if (DestPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid dest_path. Must start with /Game/."));
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
	UObject* NewAsset = AssetTools.DuplicateAsset(DestName, DestFolder, SourceBS);
	if (!NewAsset)
	{
		return MakeErrorResult(FString::Printf(TEXT("Engine failed to duplicate asset to '%s/%s'"), *DestFolder, *DestName));
	}

	// Save the new package
	FString SaveError;
	if (!SaveBlendSpaceAsset(NewAsset, SaveError))
	{
		return MakeErrorResult(SaveError);
	}

	FString TypeStr = DetectBlendSpaceType(SourceBS);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("source_path"), SourceBS->GetPathName());
	Result->SetStringField(TEXT("dest_path"), NewAsset->GetPathName());
	Result->SetStringField(TEXT("asset_type"), TypeStr);

	return MakeSuccessResult(Result, FString::Printf(TEXT("Duplicated %s to '%s'"), *TypeStr, *DestName));
}

// ============================================================================
// claireon.blendspace_delete
// ============================================================================

FString ClaireonAnimTool_BlendSpaceDelete::GetName() const { return TEXT("claireon.blendspace_delete"); }

FString ClaireonAnimTool_BlendSpaceDelete::GetDescription() const
{
	return TEXT("Delete a BlendSpace or AimOffset asset from disk. Stateless / non-session: immediate-write deletion, no open session required. Common pitfall: requires explicit confirm=true to prevent accidental deletes; referenced assets are not auto-fixed-up and may produce dangling pointers downstream.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_BlendSpaceDelete::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path of the blend space asset to delete"), true);
	S.AddBoolean(TEXT("confirm"), TEXT("Must be true to confirm deletion"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_BlendSpaceDelete::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	bool bConfirm = false;
	if (!Arguments->TryGetBoolField(TEXT("confirm"), bConfirm) || !bConfirm)
	{
		return MakeErrorResult(TEXT("Deletion not confirmed. Set 'confirm' to true to delete the asset."));
	}

	// Load the blend space to verify it exists and is the right type
	FString LoadError;
	UBlendSpace* BlendSpace = LoadBlendSpace(AssetPath, LoadError);
	if (!BlendSpace)
	{
		return MakeErrorResult(LoadError);
	}

	FString TypeStr = DetectBlendSpaceType(BlendSpace);
	FString AssetName = BlendSpace->GetName();
	FString FullPath = BlendSpace->GetPathName();

	// Check for session lock
	if (FClaireonSessionManager::Get().IsAssetLocked(FullPath))
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset '%s' is currently locked by an editing session. Close the session first."), *FullPath));
	}

	// Delete via ObjectTools
	TArray<UObject*> ObjectsToDelete;
	ObjectsToDelete.Add(BlendSpace);
	int32 Deleted = ObjectTools::ForceDeleteObjects(ObjectsToDelete, false);

	if (Deleted == 0)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to delete '%s'. It may have referencers."), *FullPath));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("deleted_path"), FullPath);
	Result->SetStringField(TEXT("asset_type"), TypeStr);
	Result->SetBoolField(TEXT("deleted"), true);

	return MakeSuccessResult(Result, FString::Printf(TEXT("Deleted %s '%s'"), *TypeStr, *AssetName));
}

// ============================================================================
// claireon.blendspace_inspect
// ============================================================================

FString ClaireonAnimTool_BlendSpaceInspect::GetName() const { return TEXT("claireon.blendspace_inspect"); }

FString ClaireonAnimTool_BlendSpaceInspect::GetDescription() const
{
	return TEXT("Inspect a BlendSpace or AimOffset asset by path. Stateless / read-only / non-session: never mutates and requires no open session. Shows axes, samples, input interpolation, settings, notify trigger mode, and metadata. Use detail_level='summary' for a compact overview or 'full' for everything.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_BlendSpaceInspect::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Unreal asset path to the blend space or aim offset"), true);
	S.AddEnum(TEXT("detail_level"), TEXT("Level of detail: 'summary' or 'full' (default: full)"),
		{TEXT("summary"), TEXT("full")});
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_BlendSpaceInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString DetailLevel = TEXT("full");
	Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel);
	bool bFullDetail = (DetailLevel != TEXT("summary"));

	FString LoadError;
	UBlendSpace* BlendSpace = LoadBlendSpace(AssetPath, LoadError);
	if (!BlendSpace)
	{
		return MakeErrorResult(LoadError);
	}

	FString InspectText = FormatBlendSpaceInspection(BlendSpace, bFullDetail);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), BlendSpace->GetPathName());
	Result->SetStringField(TEXT("asset_type"), DetectBlendSpaceType(BlendSpace));
	Result->SetStringField(TEXT("inspection"), InspectText);
	Result->SetNumberField(TEXT("sample_count"), BlendSpace->GetBlendSamples().Num());

	return MakeSuccessResult(Result, FString::Printf(TEXT("Inspected %s '%s'"), *DetectBlendSpaceType(BlendSpace), *BlendSpace->GetName()));
}

// ============================================================================
// claireon.blendspace_add_sample
// ============================================================================

FString ClaireonAnimTool_BlendSpaceAddSample::GetName() const { return TEXT("claireon.blendspace_add_sample"); }

FString ClaireonAnimTool_BlendSpaceAddSample::GetDescription() const
{
	return TEXT("Add a blend sample to a BlendSpace or AimOffset. Stateless / non-session: immediate-write to the asset by path. Specify the animation, position (x and optionally y for 2D), and an optional rate scale. Common pitfall: the animation must use the same skeleton as the blend space or this errors.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_BlendSpaceAddSample::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Blend space asset path"), true);
	S.AddString(TEXT("animation"), TEXT("AnimSequence asset path for this sample"), true);
	S.AddNumber(TEXT("x"), TEXT("Horizontal axis value (or sole axis for 1D)"), true);
	S.AddNumber(TEXT("y"), TEXT("Vertical axis value (2D blend spaces only, ignored for 1D)"));
	S.AddNumber(TEXT("rate_scale"), TEXT("Playback rate scale (default: 1.0)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_BlendSpaceAddSample::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}
	FString AnimPath;
	if (!Arguments->TryGetStringField(TEXT("animation"), AnimPath) || AnimPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: animation"));
	}
	double X = 0.0;
	if (!Arguments->TryGetNumberField(TEXT("x"), X))
	{
		return MakeErrorResult(TEXT("Missing required parameter: x"));
	}

	double Y = 0.0;
	Arguments->TryGetNumberField(TEXT("y"), Y);

	double RateScale = 1.0;
	Arguments->TryGetNumberField(TEXT("rate_scale"), RateScale);

	// Load blend space
	FString LoadError;
	UBlendSpace* BlendSpace = LoadBlendSpace(AssetPath, LoadError);
	if (!BlendSpace)
	{
		return MakeErrorResult(LoadError);
	}

	// Load animation
	auto AnimResolve = ClaireonPathResolver::Resolve(AnimPath);
	if (!AnimResolve.bSuccess)
	{
		return MakeErrorResult(AnimResolve.Error);
	}
	UAnimSequence* AnimSeq = LoadObject<UAnimSequence>(nullptr, *AnimResolve.ResolvedPath.Path);
	if (!AnimSeq)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load AnimSequence at '%s'"), *AnimResolve.ResolvedPath.Path));
	}

	// Validate skeleton compatibility
	if (AnimSeq->GetSkeleton() != BlendSpace->GetSkeleton())
	{
		return MakeErrorResult(TEXT("Animation skeleton does not match the blend space skeleton"));
	}

	// For 1D blend spaces, Y must be 0
	bool bIs1D = Is1DBlendSpace(BlendSpace);
	FVector SampleValue(X, bIs1D ? 0.0 : Y, 0.0);

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "BSAddSample", "MCP: Add BlendSpace Sample"));
	BlendSpace->Modify();

	int32 NewIndex = BlendSpace->AddSample(AnimSeq, SampleValue);
	if (NewIndex == INDEX_NONE)
	{
		return MakeErrorResult(TEXT("Failed to add sample to blend space"));
	}

	// Set rate scale if non-default
	if (!FMath::IsNearlyEqual(RateScale, 1.0))
	{
		FProperty* SampleDataProp = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("SampleData"));
		if (SampleDataProp)
		{
			TArray<FBlendSample>* SampleDataPtr = SampleDataProp->ContainerPtrToValuePtr<TArray<FBlendSample>>(BlendSpace);
			if (SampleDataPtr && SampleDataPtr->IsValidIndex(NewIndex))
			{
				(*SampleDataPtr)[NewIndex].RateScale = static_cast<float>(RateScale);
			}
		}
	}

	BlendSpace->ValidateSampleData();
	BlendSpace->ResampleData();
	BlendSpace->PostEditChange();
	BlendSpace->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(BlendSpace);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), BlendSpace->GetPathName());
	Result->SetNumberField(TEXT("sample_index"), NewIndex);
	Result->SetStringField(TEXT("animation"), AnimSeq->GetPathName());
	Result->SetNumberField(TEXT("x"), SampleValue.X);
	if (!bIs1D) Result->SetNumberField(TEXT("y"), SampleValue.Y);
	Result->SetNumberField(TEXT("rate_scale"), RateScale);
	Result->SetNumberField(TEXT("total_samples"), BlendSpace->GetBlendSamples().Num());

	return MakeSuccessResult(Result, FString::Printf(TEXT("Added sample [%d] %s at (%.2f%s)"),
		NewIndex, *AnimSeq->GetName(), SampleValue.X,
		bIs1D ? TEXT("") : *FString::Printf(TEXT(", %.2f"), SampleValue.Y)));
}

// ============================================================================
// claireon.blendspace_remove_sample
// ============================================================================

FString ClaireonAnimTool_BlendSpaceRemoveSample::GetName() const { return TEXT("claireon.blendspace_remove_sample"); }

FString ClaireonAnimTool_BlendSpaceRemoveSample::GetDescription() const
{
	return TEXT("Remove a blend sample by zero-based index from a BlendSpace or AimOffset. Stateless / non-session: immediate-write to the asset by path. Common pitfall: indices shift after removal, so cache them up front when removing multiple samples in sequence on the same asset.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_BlendSpaceRemoveSample::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Blend space asset path"), true);
	S.AddInteger(TEXT("sample_index"), TEXT("0-based index of the sample to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_BlendSpaceRemoveSample::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}
	double IndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("sample_index"), IndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: sample_index"));
	}
	int32 SampleIndex = static_cast<int32>(IndexD);

	// Load blend space
	FString LoadError;
	UBlendSpace* BlendSpace = LoadBlendSpace(AssetPath, LoadError);
	if (!BlendSpace)
	{
		return MakeErrorResult(LoadError);
	}

	// Validate index
	if (!BlendSpace->IsValidBlendSampleIndex(SampleIndex))
	{
		return MakeErrorResult(FString::Printf(TEXT("Sample index %d out of range [0, %d)"),
			SampleIndex, BlendSpace->GetNumberOfBlendSamples()));
	}

	// Capture info before deletion
	const FBlendSample& Sample = BlendSpace->GetBlendSample(SampleIndex);
	FString AnimName = Sample.Animation ? Sample.Animation->GetName() : TEXT("(none)");

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "BSRemoveSample", "MCP: Remove BlendSpace Sample"));
	BlendSpace->Modify();

	bool bDeleted = BlendSpace->DeleteSample(SampleIndex);
	if (!bDeleted)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to delete sample at index %d"), SampleIndex));
	}

	BlendSpace->ValidateSampleData();
	BlendSpace->ResampleData();
	BlendSpace->PostEditChange();
	BlendSpace->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(BlendSpace);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), BlendSpace->GetPathName());
	Result->SetNumberField(TEXT("removed_index"), SampleIndex);
	Result->SetStringField(TEXT("removed_animation"), AnimName);
	Result->SetNumberField(TEXT("remaining_samples"), BlendSpace->GetBlendSamples().Num());

	return MakeSuccessResult(Result, FString::Printf(TEXT("Removed sample [%d] %s"), SampleIndex, *AnimName));
}

// ============================================================================
// claireon.blendspace_edit_sample
// ============================================================================

FString ClaireonAnimTool_BlendSpaceEditSample::GetName() const { return TEXT("claireon.blendspace_edit_sample"); }

FString ClaireonAnimTool_BlendSpaceEditSample::GetDescription() const
{
	return TEXT("Edit an existing blend sample's position, animation, or rate scale on a BlendSpace or AimOffset. Stateless / non-session: immediate-write to the asset by path. Provide at least one of x, y, animation, or rate_scale to modify; omitted fields are left unchanged.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_BlendSpaceEditSample::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Blend space asset path"), true);
	S.AddInteger(TEXT("sample_index"), TEXT("0-based index of the sample to edit"), true);
	S.AddNumber(TEXT("x"), TEXT("New horizontal axis value"));
	S.AddNumber(TEXT("y"), TEXT("New vertical axis value (2D only)"));
	S.AddString(TEXT("animation"), TEXT("New AnimSequence asset path"));
	S.AddNumber(TEXT("rate_scale"), TEXT("New playback rate scale"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_BlendSpaceEditSample::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}
	double IndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("sample_index"), IndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: sample_index"));
	}
	int32 SampleIndex = static_cast<int32>(IndexD);

	// Load blend space
	FString LoadError;
	UBlendSpace* BlendSpace = LoadBlendSpace(AssetPath, LoadError);
	if (!BlendSpace)
	{
		return MakeErrorResult(LoadError);
	}

	// Validate index
	if (!BlendSpace->IsValidBlendSampleIndex(SampleIndex))
	{
		return MakeErrorResult(FString::Printf(TEXT("Sample index %d out of range [0, %d)"),
			SampleIndex, BlendSpace->GetNumberOfBlendSamples()));
	}

	// Check that at least one modification is provided
	bool bHasX = Arguments->HasField(TEXT("x"));
	bool bHasY = Arguments->HasField(TEXT("y"));
	bool bHasAnim = Arguments->HasField(TEXT("animation"));
	bool bHasRate = Arguments->HasField(TEXT("rate_scale"));

	if (!bHasX && !bHasY && !bHasAnim && !bHasRate)
	{
		return MakeErrorResult(TEXT("At least one of x, y, animation, or rate_scale must be provided"));
	}

	bool bIs1D = Is1DBlendSpace(BlendSpace);
	TArray<FString> Changes;

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "BSEditSample", "MCP: Edit BlendSpace Sample"));
	BlendSpace->Modify();

	// Edit position if x or y provided
	if (bHasX || bHasY)
	{
		const FBlendSample& CurrentSample = BlendSpace->GetBlendSample(SampleIndex);
		FVector NewValue = CurrentSample.SampleValue;

		double XVal = 0.0;
		if (bHasX && Arguments->TryGetNumberField(TEXT("x"), XVal))
		{
			NewValue.X = XVal;
		}
		double YVal = 0.0;
		if (bHasY && !bIs1D && Arguments->TryGetNumberField(TEXT("y"), YVal))
		{
			NewValue.Y = YVal;
		}
		if (bIs1D)
		{
			NewValue.Y = 0.0;
		}

		BlendSpace->EditSampleValue(SampleIndex, NewValue);
		if (bIs1D)
		{
			Changes.Add(FString::Printf(TEXT("position -> X=%.2f"), NewValue.X));
		}
		else
		{
			Changes.Add(FString::Printf(TEXT("position -> X=%.2f Y=%.2f"), NewValue.X, NewValue.Y));
		}
	}

	// Replace animation if provided
	if (bHasAnim)
	{
		FString AnimPath;
		Arguments->TryGetStringField(TEXT("animation"), AnimPath);
		if (!AnimPath.IsEmpty())
		{
			auto AnimResolve = ClaireonPathResolver::Resolve(AnimPath);
			if (!AnimResolve.bSuccess)
			{
				return MakeErrorResult(AnimResolve.Error);
			}
			UAnimSequence* NewAnim = LoadObject<UAnimSequence>(nullptr, *AnimResolve.ResolvedPath.Path);
			if (!NewAnim)
			{
				return MakeErrorResult(FString::Printf(TEXT("Failed to load AnimSequence at '%s'"), *AnimResolve.ResolvedPath.Path));
			}
			if (NewAnim->GetSkeleton() != BlendSpace->GetSkeleton())
			{
				return MakeErrorResult(TEXT("New animation skeleton does not match the blend space skeleton"));
			}

			BlendSpace->ReplaceSampleAnimation(SampleIndex, NewAnim);
			Changes.Add(FString::Printf(TEXT("animation -> %s"), *NewAnim->GetName()));
		}
	}

	// Edit rate scale via reflection
	if (bHasRate)
	{
		double RateScale = 1.0;
		Arguments->TryGetNumberField(TEXT("rate_scale"), RateScale);

		FProperty* SampleDataProp = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("SampleData"));
		if (SampleDataProp)
		{
			TArray<FBlendSample>* SampleDataPtr = SampleDataProp->ContainerPtrToValuePtr<TArray<FBlendSample>>(BlendSpace);
			if (SampleDataPtr && SampleDataPtr->IsValidIndex(SampleIndex))
			{
				(*SampleDataPtr)[SampleIndex].RateScale = static_cast<float>(RateScale);
				Changes.Add(FString::Printf(TEXT("rate_scale -> %.2f"), RateScale));
			}
		}
	}

	BlendSpace->ValidateSampleData();
	BlendSpace->ResampleData();
	BlendSpace->PostEditChange();
	BlendSpace->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(BlendSpace);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), BlendSpace->GetPathName());
	Result->SetNumberField(TEXT("sample_index"), SampleIndex);

	// Report the final state of the sample
	const FBlendSample& FinalSample = BlendSpace->GetBlendSample(SampleIndex);
	Result->SetNumberField(TEXT("x"), FinalSample.SampleValue.X);
	if (!bIs1D) Result->SetNumberField(TEXT("y"), FinalSample.SampleValue.Y);
	Result->SetStringField(TEXT("animation"), FinalSample.Animation ? FinalSample.Animation->GetPathName() : TEXT("(none)"));
	Result->SetNumberField(TEXT("rate_scale"), FinalSample.RateScale);

	FString ChangeSummary = FString::Join(Changes, TEXT(", "));
	return MakeSuccessResult(Result, FString::Printf(TEXT("Edited sample [%d]: %s"), SampleIndex, *ChangeSummary));
}

// ============================================================================
// claireon.blendspace_set_axis
// ============================================================================

FString ClaireonAnimTool_BlendSpaceSetAxis::GetName() const { return TEXT("claireon.blendspace_set_axis"); }

FString ClaireonAnimTool_BlendSpaceSetAxis::GetDescription() const
{
	return TEXT("Configure a blend space axis on a BlendSpace or AimOffset. Stateless / non-session: immediate-write to the asset by path. Set display name, value range, grid divisions, snap-to-grid, and input wrapping. Axis 0 is horizontal (X), axis 1 is vertical (Y); 1D blend spaces only accept axis 0.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_BlendSpaceSetAxis::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Blend space asset path"), true);
	S.AddInteger(TEXT("axis"), TEXT("Axis index: 0 (horizontal) or 1 (vertical, 2D only)"), true);
	S.AddString(TEXT("display_name"), TEXT("Human-readable axis name (e.g. Speed, Direction, Yaw, Pitch)"));
	S.AddNumber(TEXT("min"), TEXT("Minimum axis value"));
	S.AddNumber(TEXT("max"), TEXT("Maximum axis value"));
	S.AddInteger(TEXT("grid_divisions"), TEXT("Number of grid divisions"));
	S.AddBoolean(TEXT("snap_to_grid"), TEXT("Whether samples snap to grid points"));
	S.AddBoolean(TEXT("wrap_input"), TEXT("Whether input wraps around (cyclic axis)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_BlendSpaceSetAxis::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}
	double AxisD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("axis"), AxisD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: axis"));
	}
	int32 AxisIndex = static_cast<int32>(AxisD);

	// Load blend space
	FString LoadError;
	UBlendSpace* BlendSpace = LoadBlendSpace(AssetPath, LoadError);
	if (!BlendSpace)
	{
		return MakeErrorResult(LoadError);
	}

	// Validate axis
	FString AxisError;
	if (!ValidateAxisIndex(BlendSpace, AxisIndex, AxisError))
	{
		return MakeErrorResult(AxisError);
	}

	// Get mutable blend parameter via reflection
	FBlendParameter* Param = GetBlendParameterMutable(BlendSpace, AxisIndex);
	if (!Param)
	{
		return MakeErrorResult(TEXT("Failed to access BlendParameters via reflection"));
	}

	TArray<FString> Changes;

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "BSSetAxis", "MCP: Set BlendSpace Axis"));
	BlendSpace->Modify();

	FString DisplayName;
	if (Arguments->TryGetStringField(TEXT("display_name"), DisplayName))
	{
		Param->DisplayName = DisplayName;
		Changes.Add(FString::Printf(TEXT("display_name -> %s"), *DisplayName));
	}

	double MinVal = 0.0;
	if (Arguments->TryGetNumberField(TEXT("min"), MinVal))
	{
		Param->Min = static_cast<float>(MinVal);
		Changes.Add(FString::Printf(TEXT("min -> %.2f"), MinVal));
	}

	double MaxVal = 0.0;
	if (Arguments->TryGetNumberField(TEXT("max"), MaxVal))
	{
		Param->Max = static_cast<float>(MaxVal);
		Changes.Add(FString::Printf(TEXT("max -> %.2f"), MaxVal));
	}

	double GridDiv = 0.0;
	if (Arguments->TryGetNumberField(TEXT("grid_divisions"), GridDiv))
	{
		Param->GridNum = static_cast<int32>(GridDiv);
		Changes.Add(FString::Printf(TEXT("grid_divisions -> %d"), Param->GridNum));
	}

	bool bSnapToGrid = false;
	if (Arguments->TryGetBoolField(TEXT("snap_to_grid"), bSnapToGrid))
	{
		Param->bSnapToGrid = bSnapToGrid;
		Changes.Add(FString::Printf(TEXT("snap_to_grid -> %s"), bSnapToGrid ? TEXT("true") : TEXT("false")));
	}

	bool bWrapInput = false;
	if (Arguments->TryGetBoolField(TEXT("wrap_input"), bWrapInput))
	{
		Param->bWrapInput = bWrapInput;
		Changes.Add(FString::Printf(TEXT("wrap_input -> %s"), bWrapInput ? TEXT("true") : TEXT("false")));
	}

	if (Changes.Num() == 0)
	{
		return MakeErrorResult(TEXT("No axis properties specified to change"));
	}

	BlendSpace->ValidateSampleData();
	BlendSpace->ResampleData();
	BlendSpace->PostEditChange();
	BlendSpace->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(BlendSpace);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), BlendSpace->GetPathName());
	Result->SetNumberField(TEXT("axis"), AxisIndex);
	Result->SetStringField(TEXT("display_name"), Param->DisplayName);
	Result->SetNumberField(TEXT("min"), Param->Min);
	Result->SetNumberField(TEXT("max"), Param->Max);
	Result->SetNumberField(TEXT("grid_divisions"), Param->GridNum);
	Result->SetBoolField(TEXT("snap_to_grid"), Param->bSnapToGrid);
	Result->SetBoolField(TEXT("wrap_input"), Param->bWrapInput);

	FString ChangeSummary = FString::Join(Changes, TEXT(", "));
	return MakeSuccessResult(Result, FString::Printf(TEXT("Set axis %d: %s"), AxisIndex, *ChangeSummary));
}

// ============================================================================
// claireon.blendspace_set_interpolation
// ============================================================================

FString ClaireonAnimTool_BlendSpaceSetInterpolation::GetName() const { return TEXT("claireon.blendspace_set_interpolation"); }

FString ClaireonAnimTool_BlendSpaceSetInterpolation::GetDescription() const
{
	return TEXT("Configure input interpolation (smoothing) for a blend space axis. Stateless / non-session: immediate-write to the asset by path. Controls how quickly the blend space transitions between input values; tune to match how player input feeds the blend axis at runtime.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_BlendSpaceSetInterpolation::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Blend space asset path"), true);
	S.AddInteger(TEXT("axis"), TEXT("Axis index: 0, 1, or 2"), true);
	S.AddNumber(TEXT("smoothing_time"), TEXT("Smoothing time in seconds (0 = no smoothing)"));
	S.AddNumber(TEXT("damping_ratio"), TEXT("Damping ratio for SpringDamper type (default: 1.0)"));
	S.AddNumber(TEXT("max_speed"), TEXT("Maximum speed limit (0 = unlimited)"));
	S.AddEnum(TEXT("smoothing_type"), TEXT("Interpolation type"),
		{TEXT("Average"), TEXT("Linear"), TEXT("Cubic"), TEXT("EaseInOut"), TEXT("SpringDamper"), TEXT("Exponential")});
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_BlendSpaceSetInterpolation::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}
	double AxisD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("axis"), AxisD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: axis"));
	}
	int32 AxisIndex = static_cast<int32>(AxisD);

	// InterpolationParam is a fixed-size array [3], so 0-2 are valid
	if (AxisIndex < 0 || AxisIndex > 2)
	{
		return MakeErrorResult(FString::Printf(TEXT("Axis index %d out of range [0, 2]"), AxisIndex));
	}

	// Load blend space
	FString LoadError;
	UBlendSpace* BlendSpace = LoadBlendSpace(AssetPath, LoadError);
	if (!BlendSpace)
	{
		return MakeErrorResult(LoadError);
	}

	TArray<FString> Changes;

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "BSSetInterp", "MCP: Set BlendSpace Interpolation"));
	BlendSpace->Modify();

	FInterpolationParameter& InterpParam = BlendSpace->InterpolationParam[AxisIndex];

	double SmoothingTime = 0.0;
	if (Arguments->TryGetNumberField(TEXT("smoothing_time"), SmoothingTime))
	{
		InterpParam.InterpolationTime = static_cast<float>(SmoothingTime);
		Changes.Add(FString::Printf(TEXT("smoothing_time -> %.3f"), SmoothingTime));
	}

	double DampingRatio = 0.0;
	if (Arguments->TryGetNumberField(TEXT("damping_ratio"), DampingRatio))
	{
		InterpParam.DampingRatio = static_cast<float>(DampingRatio);
		Changes.Add(FString::Printf(TEXT("damping_ratio -> %.3f"), DampingRatio));
	}

	double MaxSpeed = 0.0;
	if (Arguments->TryGetNumberField(TEXT("max_speed"), MaxSpeed))
	{
		InterpParam.MaxSpeed = static_cast<float>(MaxSpeed);
		Changes.Add(FString::Printf(TEXT("max_speed -> %.1f"), MaxSpeed));
	}

	FString SmoothingType;
	if (Arguments->TryGetStringField(TEXT("smoothing_type"), SmoothingType) && !SmoothingType.IsEmpty())
	{
		bool bValid = false;
		EFilterInterpolationType ParsedType = ParseSmoothingType(SmoothingType, bValid);
		if (!bValid)
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid smoothing_type '%s'. Must be one of: Average, Linear, Cubic, EaseInOut, SpringDamper, Exponential"), *SmoothingType));
		}
		InterpParam.InterpolationType = ParsedType;
		Changes.Add(FString::Printf(TEXT("smoothing_type -> %s"), *SmoothingType));
	}

	if (Changes.Num() == 0)
	{
		return MakeErrorResult(TEXT("No interpolation properties specified to change"));
	}

	BlendSpace->PostEditChange();
	BlendSpace->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(BlendSpace);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), BlendSpace->GetPathName());
	Result->SetNumberField(TEXT("axis"), AxisIndex);
	Result->SetNumberField(TEXT("smoothing_time"), InterpParam.InterpolationTime);
	Result->SetNumberField(TEXT("damping_ratio"), InterpParam.DampingRatio);
	Result->SetNumberField(TEXT("max_speed"), InterpParam.MaxSpeed);

	FString ChangeSummary = FString::Join(Changes, TEXT(", "));
	return MakeSuccessResult(Result, FString::Printf(TEXT("Set interpolation axis %d: %s"), AxisIndex, *ChangeSummary));
}

// ============================================================================
// claireon.blendspace_set_property
// ============================================================================

FString ClaireonAnimTool_BlendSpaceSetProperty::GetName() const { return TEXT("claireon.blendspace_set_property"); }

FString ClaireonAnimTool_BlendSpaceSetProperty::GetDescription() const
{
	return TEXT("Set general blend space properties on a BlendSpace or AimOffset. Stateless / non-session: immediate-write to the asset by path. Supported keys: loop, notify_trigger_mode, target_weight_speed/easing, allow_mesh_space_blending, allow_marker_sync, use_grid_interpolation, preferred_triangulation_direction, axis_to_scale_animation, scale_animation (1D only).");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_BlendSpaceSetProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Blend space asset path"), true);
	S.AddObject(TEXT("properties"), TEXT("Key-value map of properties to set (see tool description for supported keys)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_BlendSpaceSetProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	const TSharedPtr<FJsonObject>* PropertiesPtr = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("properties"), PropertiesPtr) || !PropertiesPtr || !PropertiesPtr->IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: properties"));
	}
	const TSharedPtr<FJsonObject>& Properties = *PropertiesPtr;

	// Load blend space
	FString LoadError;
	UBlendSpace* BlendSpace = LoadBlendSpace(AssetPath, LoadError);
	if (!BlendSpace)
	{
		return MakeErrorResult(LoadError);
	}

	TArray<FString> Changes;
	TArray<FString> Errors;

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "BSSetProp", "MCP: Set BlendSpace Property"));
	BlendSpace->Modify();

	// loop
	bool bLoop = false;
	if (Properties->TryGetBoolField(TEXT("loop"), bLoop))
	{
		BlendSpace->bLoop = bLoop;
		Changes.Add(FString::Printf(TEXT("loop -> %s"), bLoop ? TEXT("true") : TEXT("false")));
	}

	// notify_trigger_mode
	FString NotifyMode;
	if (Properties->TryGetStringField(TEXT("notify_trigger_mode"), NotifyMode) && !NotifyMode.IsEmpty())
	{
		if (NotifyMode == TEXT("AllAnimations"))
		{
			BlendSpace->NotifyTriggerMode = ENotifyTriggerMode::AllAnimations;
			Changes.Add(TEXT("notify_trigger_mode -> AllAnimations"));
		}
		else if (NotifyMode == TEXT("HighestWeightedAnimation"))
		{
			BlendSpace->NotifyTriggerMode = ENotifyTriggerMode::HighestWeightedAnimation;
			Changes.Add(TEXT("notify_trigger_mode -> HighestWeightedAnimation"));
		}
		else if (NotifyMode == TEXT("None"))
		{
			BlendSpace->NotifyTriggerMode = ENotifyTriggerMode::None;
			Changes.Add(TEXT("notify_trigger_mode -> None"));
		}
		else
		{
			Errors.Add(FString::Printf(TEXT("Invalid notify_trigger_mode '%s'"), *NotifyMode));
		}
	}

	// target_weight_speed
	double TargetWeightSpeed = 0.0;
	if (Properties->TryGetNumberField(TEXT("target_weight_speed"), TargetWeightSpeed))
	{
		BlendSpace->TargetWeightInterpolationSpeedPerSec = static_cast<float>(TargetWeightSpeed);
		Changes.Add(FString::Printf(TEXT("target_weight_speed -> %.2f"), TargetWeightSpeed));
	}

	// target_weight_easing
	bool bEasing = false;
	if (Properties->TryGetBoolField(TEXT("target_weight_easing"), bEasing))
	{
		BlendSpace->bTargetWeightInterpolationEaseInOut = bEasing;
		Changes.Add(FString::Printf(TEXT("target_weight_easing -> %s"), bEasing ? TEXT("true") : TEXT("false")));
	}

	// allow_mesh_space_blending
	bool bMeshSpace = false;
	if (Properties->TryGetBoolField(TEXT("allow_mesh_space_blending"), bMeshSpace))
	{
		BlendSpace->bAllowMeshSpaceBlending = bMeshSpace;
		Changes.Add(FString::Printf(TEXT("allow_mesh_space_blending -> %s"), bMeshSpace ? TEXT("true") : TEXT("false")));
	}

	// allow_marker_sync
	bool bMarkerSync = false;
	if (Properties->TryGetBoolField(TEXT("allow_marker_sync"), bMarkerSync))
	{
		BlendSpace->bAllowMarkerBasedSync = bMarkerSync;
		Changes.Add(FString::Printf(TEXT("allow_marker_sync -> %s"), bMarkerSync ? TEXT("true") : TEXT("false")));
	}

	// use_grid_interpolation
	bool bGridInterp = false;
	if (Properties->TryGetBoolField(TEXT("use_grid_interpolation"), bGridInterp))
	{
		BlendSpace->bInterpolateUsingGrid = bGridInterp;
		Changes.Add(FString::Printf(TEXT("use_grid_interpolation -> %s"), bGridInterp ? TEXT("true") : TEXT("false")));
	}

	// preferred_triangulation_direction
	FString TriDir;
	if (Properties->TryGetStringField(TEXT("preferred_triangulation_direction"), TriDir) && !TriDir.IsEmpty())
	{
		if (TriDir == TEXT("None"))
		{
			BlendSpace->PreferredTriangulationDirection = EPreferredTriangulationDirection::None;
			Changes.Add(TEXT("preferred_triangulation_direction -> None"));
		}
		else if (TriDir == TEXT("Tangential"))
		{
			BlendSpace->PreferredTriangulationDirection = EPreferredTriangulationDirection::Tangential;
			Changes.Add(TEXT("preferred_triangulation_direction -> Tangential"));
		}
		else if (TriDir == TEXT("Radial"))
		{
			BlendSpace->PreferredTriangulationDirection = EPreferredTriangulationDirection::Radial;
			Changes.Add(TEXT("preferred_triangulation_direction -> Radial"));
		}
		else
		{
			Errors.Add(FString::Printf(TEXT("Invalid preferred_triangulation_direction '%s' (expected None, Tangential, or Radial)"), *TriDir));
		}
	}

	// axis_to_scale_animation (protected, use reflection)
	FString ScaleAxis;
	if (Properties->TryGetStringField(TEXT("axis_to_scale_animation"), ScaleAxis) && !ScaleAxis.IsEmpty())
	{
		FProperty* ScaleAxisProp = UBlendSpace::StaticClass()->FindPropertyByName(TEXT("AxisToScaleAnimation"));
		if (ScaleAxisProp)
		{
			TEnumAsByte<EBlendSpaceAxis>* AxisPtr = ScaleAxisProp->ContainerPtrToValuePtr<TEnumAsByte<EBlendSpaceAxis>>(BlendSpace);
			if (AxisPtr)
			{
				if (ScaleAxis == TEXT("None"))
				{
					*AxisPtr = BSA_None;
					Changes.Add(TEXT("axis_to_scale_animation -> None"));
				}
				else if (ScaleAxis == TEXT("X"))
				{
					*AxisPtr = BSA_X;
					Changes.Add(TEXT("axis_to_scale_animation -> X"));
				}
				else if (ScaleAxis == TEXT("Y"))
				{
					*AxisPtr = BSA_Y;
					Changes.Add(TEXT("axis_to_scale_animation -> Y"));
				}
				else
				{
					Errors.Add(FString::Printf(TEXT("Invalid axis_to_scale_animation '%s' (expected None, X, or Y)"), *ScaleAxis));
				}
			}
		}
	}

	// scale_animation (BlendSpace1D only)
	bool bScaleAnim = false;
	if (Properties->TryGetBoolField(TEXT("scale_animation"), bScaleAnim))
	{
		UBlendSpace1D* BS1D = Cast<UBlendSpace1D>(BlendSpace);
		if (BS1D)
		{
			BS1D->bScaleAnimation = bScaleAnim;
			Changes.Add(FString::Printf(TEXT("scale_animation -> %s"), bScaleAnim ? TEXT("true") : TEXT("false")));
		}
		else
		{
			Errors.Add(TEXT("scale_animation is only valid on BlendSpace1D"));
		}
	}

	if (Changes.Num() == 0 && Errors.Num() == 0)
	{
		return MakeErrorResult(TEXT("No recognized properties specified"));
	}

	BlendSpace->PostEditChange();
	BlendSpace->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(BlendSpace);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), BlendSpace->GetPathName());
	Result->SetNumberField(TEXT("changes_applied"), Changes.Num());

	FString ChangeSummary = FString::Join(Changes, TEXT(", "));
	if (!ChangeSummary.IsEmpty())
	{
		Result->SetStringField(TEXT("changes"), ChangeSummary);
	}

	FToolResult ToolResult = MakeSuccessResult(Result,
		FString::Printf(TEXT("Set %d properties on %s"), Changes.Num(), *BlendSpace->GetName()));

	// Add any errors as warnings
	for (const FString& Err : Errors)
	{
		ToolResult.Warnings.Add(Err);
	}

	return ToolResult;
}

// ============================================================================
// claireon.blendspace_add_metadata
// ============================================================================

FString ClaireonAnimTool_BlendSpaceAddMetadata::GetName() const { return TEXT("claireon.blendspace_add_metadata"); }

FString ClaireonAnimTool_BlendSpaceAddMetadata::GetDescription() const
{
	return TEXT("Add a metadata object to a BlendSpace or AimOffset asset by class name. Stateless / non-session: immediate-write to the asset by path. The metadata_class must be a UAnimMetaData subclass; new entries append to the asset's metadata array. Configure properties on the BlendSpace asset directly post-add.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_BlendSpaceAddMetadata::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Blend space asset path"), true);
	S.AddString(TEXT("class_name"), TEXT("Metadata class name (native or Blueprint path)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_BlendSpaceAddMetadata::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}
	FString ClassName;
	if (!Arguments->TryGetStringField(TEXT("class_name"), ClassName) || ClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: class_name"));
	}

	// Load blend space
	FString LoadError;
	UBlendSpace* BlendSpace = LoadBlendSpace(AssetPath, LoadError);
	if (!BlendSpace)
	{
		return MakeErrorResult(LoadError);
	}

	// Resolve metadata class
	ClaireonNameResolver::FNameResolveResult NameResult;
	UClass* MetaDataClass = ClaireonNameResolver::ResolveClassName(ClassName, UAnimMetaData::StaticClass(), NameResult);
	if (!MetaDataClass)
	{
		return MakeErrorResult(NameResult.Error);
	}

	if (MetaDataClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return MakeErrorResult(FString::Printf(TEXT("Class '%s' is abstract and cannot be instantiated. Use a concrete subclass."), *ClassName));
	}

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "BSAddMetadata", "MCP: Add BlendSpace Metadata"));
	BlendSpace->Modify();

	UAnimMetaData* NewMetaData = NewObject<UAnimMetaData>(BlendSpace, MetaDataClass);
	if (!NewMetaData)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create metadata of class %s"), *ClassName));
	}

	BlendSpace->AddMetaData(NewMetaData);

	BlendSpace->PostEditChange();
	BlendSpace->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(BlendSpace);

	int32 NewIndex = BlendSpace->GetMetaData().Num() - 1;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), BlendSpace->GetPathName());
	Result->SetStringField(TEXT("class_name"), MetaDataClass->GetName());
	Result->SetNumberField(TEXT("metadata_index"), NewIndex);
	Result->SetNumberField(TEXT("total_metadata"), BlendSpace->GetMetaData().Num());

	return MakeSuccessResult(Result, FString::Printf(TEXT("Added metadata %s [%d]"), *MetaDataClass->GetName(), NewIndex));
}

// ============================================================================
// claireon.blendspace_remove_metadata
// ============================================================================

FString ClaireonAnimTool_BlendSpaceRemoveMetadata::GetName() const { return TEXT("claireon.blendspace_remove_metadata"); }

FString ClaireonAnimTool_BlendSpaceRemoveMetadata::GetDescription() const
{
	return TEXT("Remove a metadata object from a BlendSpace or AimOffset by zero-based index. Stateless / non-session: immediate-write to the asset by path. Common pitfall: indices shift after removal, so cache them up front when removing multiple metadata entries in sequence on the same asset.");
}

TSharedPtr<FJsonObject> ClaireonAnimTool_BlendSpaceRemoveMetadata::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Blend space asset path"), true);
	S.AddInteger(TEXT("metadata_index"), TEXT("0-based index of the metadata object to remove"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonAnimTool_BlendSpaceRemoveMetadata::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}
	double IndexD = -1.0;
	if (!Arguments->TryGetNumberField(TEXT("metadata_index"), IndexD))
	{
		return MakeErrorResult(TEXT("Missing required parameter: metadata_index"));
	}
	int32 MetadataIndex = static_cast<int32>(IndexD);

	// Load blend space
	FString LoadError;
	UBlendSpace* BlendSpace = LoadBlendSpace(AssetPath, LoadError);
	if (!BlendSpace)
	{
		return MakeErrorResult(LoadError);
	}

	const TArray<UAnimMetaData*>& MetaDataArray = BlendSpace->GetMetaData();
	if (MetadataIndex < 0 || MetadataIndex >= MetaDataArray.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Metadata index %d out of range [0, %d)"),
			MetadataIndex, MetaDataArray.Num()));
	}

	UAnimMetaData* MetaDataObj = MetaDataArray[MetadataIndex];
	if (!MetaDataObj)
	{
		return MakeErrorResult(FString::Printf(TEXT("Metadata at index %d is null"), MetadataIndex));
	}

	FString MetaDataName = MetaDataObj->GetClass()->GetName();

	FScopedTransaction Transaction(NSLOCTEXT("Claireon", "BSRemoveMetadata", "MCP: Remove BlendSpace Metadata"));
	BlendSpace->Modify();

	BlendSpace->RemoveMetaData(MetaDataObj);

	BlendSpace->PostEditChange();
	BlendSpace->MarkPackageDirty();
	ClaireonAssetUtils::RefreshAssetEditorIfOpen(BlendSpace);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), BlendSpace->GetPathName());
	Result->SetNumberField(TEXT("removed_index"), MetadataIndex);
	Result->SetStringField(TEXT("removed_class"), MetaDataName);
	Result->SetNumberField(TEXT("remaining_metadata"), BlendSpace->GetMetaData().Num());

	return MakeSuccessResult(Result, FString::Printf(TEXT("Removed metadata %s [%d]"), *MetaDataName, MetadataIndex));
}
