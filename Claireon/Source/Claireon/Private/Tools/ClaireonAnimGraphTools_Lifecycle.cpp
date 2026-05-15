// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimGraphTools_Lifecycle.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "Tools/ClaireonAnimGraphHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonPathResolver.h"
#include "ClaireonNameResolver.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLog.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Animation/AnimInstance.h"
#include "Animation/Skeleton.h"
#include "Engine/SkeletalMesh.h"

#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/SavePackage.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

#define LOCTEXT_NAMESPACE "ClaireonAnimGraphTools_Lifecycle"

// ============================================================================
// ClaireonAnimGraphTool_Create
// ============================================================================

FString ClaireonAnimGraphTool_Create::GetName() const
{
	return TEXT("claireon.animgraph_create");
}

FString ClaireonAnimGraphTool_Create::GetDescription() const
{
	return TEXT("Create a new Animation Blueprint from scratch, optionally as a skeleton-agnostic template.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_Create::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Destination path for the new AnimBP (e.g., /Game/Test/ABP_MyAnim)"), true);
	S.AddString(TEXT("skeleton_path"), TEXT("Path to the USkeleton asset (required unless is_template=true)"));
	S.AddString(TEXT("parent_class"), TEXT("Parent class name (default: AnimInstance). Must derive from UAnimInstance."));
	S.AddBoolean(TEXT("is_template"), TEXT("If true, create a skeleton-agnostic template AnimBP (no skeleton required)"));
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	// Check if template
	bool bIsTemplate = false;
	Arguments->TryGetBoolField(TEXT("is_template"), bIsTemplate);

	// Resolve skeleton (not required for templates)
	USkeleton* Skeleton = nullptr;
	FString SkeletonPath;
	if (Arguments->TryGetStringField(TEXT("skeleton_path"), SkeletonPath))
	{
		auto SkeletonResolve = ClaireonPathResolver::Resolve(SkeletonPath);
		if (!SkeletonResolve.bSuccess)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to resolve skeleton path: %s"), *SkeletonResolve.Error));
		}

		Skeleton = Cast<USkeleton>(FSoftObjectPath(SkeletonResolve.ResolvedPath.Path).TryLoad());
	}
	else if (!bIsTemplate)
	{
		return MakeErrorResult(TEXT("Missing required field: skeleton_path (required unless is_template=true)"));
	}
	if (!Skeleton && !bIsTemplate)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load skeleton at: %s"), *SkeletonPath));
	}

	// Resolve parent class (default: UAnimInstance)
	UClass* ParentClass = UAnimInstance::StaticClass();
	FString ParentClassName;
	TArray<FString> Warnings;
	if (Arguments->TryGetStringField(TEXT("parent_class"), ParentClassName))
	{
		ClaireonNameResolver::FNameResolveResult ResolveResult;
		UClass* ResolvedClass = ClaireonNameResolver::ResolveClassName(ParentClassName, nullptr, ResolveResult);
		if (!ResolvedClass)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to resolve parent class '%s': %s"), *ParentClassName, *ResolveResult.Error));
		}
		if (!ResolvedClass->IsChildOf(UAnimInstance::StaticClass()))
		{
			return MakeErrorResult(FString::Printf(TEXT("Parent class '%s' does not derive from UAnimInstance"), *ParentClassName));
		}
		ParentClass = ResolvedClass;
		if (!ResolveResult.ResolutionNote.IsEmpty())
		{
			Warnings.Add(ResolveResult.ResolutionNote);
		}
	}

	// Extract package and asset name
	FString PackageName = AssetPath;
	FString AssetName;
	if (AssetPath.Contains(TEXT(".")))
	{
		AssetPath.Split(TEXT("."), &PackageName, &AssetName);
	}
	else
	{
		int32 LastSlash;
		if (PackageName.FindLastChar('/', LastSlash))
		{
			AssetName = PackageName.Mid(LastSlash + 1);
		}
		else
		{
			AssetName = TEXT("NewAnimBlueprint");
		}
	}

	// Delete existing file if present
	FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (FPaths::FileExists(PackageFileName))
	{
		IFileManager::Get().Delete(*PackageFileName, false, true);
	}

	// Create package
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
	}

	// Create AnimBP (mirrors UAnimBlueprintFactory::FactoryCreateNew)
	UAnimBlueprint* AnimBP = CastChecked<UAnimBlueprint>(
		FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UAnimBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None));

	if (!AnimBP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create Animation Blueprint at %s"), *PackageName));
	}

	// Set template flag and skeleton (matches UAnimBlueprintFactory::FactoryCreateNew)
	if (bIsTemplate)
	{
		AnimBP->bIsTemplate = true;
		AnimBP->TargetSkeleton = nullptr;
	}
	else
	{
		AnimBP->bIsTemplate = false;
		AnimBP->TargetSkeleton = Skeleton;
		if (UAnimBlueprintGeneratedClass* GenClass = Cast<UAnimBlueprintGeneratedClass>(AnimBP->GeneratedClass))
		{
			GenClass->TargetSkeleton = Skeleton;
		}
		if (UAnimBlueprintGeneratedClass* SkelClass = Cast<UAnimBlueprintGeneratedClass>(AnimBP->SkeletonGeneratedClass))
		{
			SkelClass->TargetSkeleton = Skeleton;
		}

		// Set preview mesh from skeleton
		if (Skeleton)
		{
			if (USkeletalMesh* PreviewMesh = Skeleton->GetPreviewMesh())
			{
				AnimBP->SetPreviewMesh(PreviewMesh);
			}
		}
	}

	// Configure package
	Package->SetIsExternallyReferenceable(true);
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(AnimBP);

	// Save to disk (direct save — ClaireonAssetUtils::SaveAsset uses CDO lookup which fails for new assets)
	{
		FString PkgFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		bool bSaveOk = UPackage::SavePackage(Package, AnimBP, *PkgFileName, SaveArgs);
		if (!bSaveOk)
		{
			Warnings.Add(FString::Printf(TEXT("AnimBP created but save failed for package: %s"), *PackageName));
		}
	}

	// Build response
	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), AnimBP->GetPathName());
	Result->SetStringField(TEXT("asset_name"), AnimBP->GetName());
	Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Result->SetBoolField(TEXT("is_template"), AnimBP->bIsTemplate);
	Result->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : TEXT("None (template)"));

	// List graphs
	TArray<ClaireonAnimGraphHelpers::FAnimGraphInfo> Graphs = ClaireonAnimGraphHelpers::CollectAllGraphs(AnimBP);
	TArray<TSharedPtr<FJsonValue>> GraphsArray;
	for (const auto& Info : Graphs)
	{
		TSharedPtr<FJsonObject> GraphObj = MakeShared<FJsonObject>();
		GraphObj->SetStringField(TEXT("name"), Info.Name);
		GraphObj->SetStringField(TEXT("type"), Info.Type);
		GraphObj->SetNumberField(TEXT("node_count"), Info.NodeCount);
		GraphsArray.Add(MakeShared<FJsonValueObject>(GraphObj));
	}
	Result->SetArrayField(TEXT("graphs"), GraphsArray);
	Result->SetNumberField(TEXT("graph_count"), Graphs.Num());

	FToolResult ToolResult = MakeSuccessResult(Result, FString::Printf(TEXT("Created Animation Blueprint: %s"), *AnimBP->GetPathName()));
	ToolResult.Warnings = Warnings;
	return ToolResult;
}

// ============================================================================
// ClaireonAnimGraphTool_CreateChild
// ============================================================================

FString ClaireonAnimGraphTool_CreateChild::GetName() const
{
	return TEXT("claireon.animgraph_create_child");
}

FString ClaireonAnimGraphTool_CreateChild::GetDescription() const
{
	return TEXT("Create a child Animation Blueprint that inherits from an existing AnimBP.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_CreateChild::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Destination path for the new child AnimBP"), true);
	S.AddString(TEXT("parent_animgraph_path"), TEXT("Path to the parent Animation Blueprint to inherit from"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_CreateChild::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath))
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(AssetPath, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	FString ParentPath;
	if (!Arguments->TryGetStringField(TEXT("parent_animgraph_path"), ParentPath))
	{
		return MakeErrorResult(TEXT("Missing required field: parent_animgraph_path"));
	}

	// Load parent AnimBP
	FString LoadError;
	UAnimBlueprint* ParentBP = ClaireonAnimGraphHelpers::LoadAnimBlueprint(ParentPath, LoadError);
	if (!ParentBP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load parent AnimBP: %s"), *LoadError));
	}

	// Get parent's generated class for inheritance
	UClass* ParentClass = ParentBP->GeneratedClass;
	if (!ParentClass)
	{
		// Compile parent if needed
		FKismetEditorUtilities::CompileBlueprint(ParentBP);
		ParentClass = ParentBP->GeneratedClass;
	}
	if (!ParentClass)
	{
		return MakeErrorResult(TEXT("Parent AnimBP has no generated class. Ensure it compiles successfully."));
	}

	// Extract package and asset name
	FString PackageName = AssetPath;
	FString AssetName;
	if (AssetPath.Contains(TEXT(".")))
	{
		AssetPath.Split(TEXT("."), &PackageName, &AssetName);
	}
	else
	{
		int32 LastSlash;
		if (PackageName.FindLastChar('/', LastSlash))
		{
			AssetName = PackageName.Mid(LastSlash + 1);
		}
		else
		{
			AssetName = TEXT("NewChildAnimBlueprint");
		}
	}

	// Delete existing file
	FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (FPaths::FileExists(PackageFileName))
	{
		IFileManager::Get().Delete(*PackageFileName, false, true);
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create package: %s"), *PackageName));
	}

	// Create child AnimBP using parent's generated class
	UAnimBlueprint* ChildBP = CastChecked<UAnimBlueprint>(
		FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UAnimBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None));

	if (!ChildBP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create child Animation Blueprint at %s"), *PackageName));
	}

	// Inherit skeleton from parent
	ChildBP->TargetSkeleton = ParentBP->TargetSkeleton;
	if (UAnimBlueprintGeneratedClass* GenClass = Cast<UAnimBlueprintGeneratedClass>(ChildBP->GeneratedClass))
	{
		GenClass->TargetSkeleton = ParentBP->TargetSkeleton;
	}
	if (UAnimBlueprintGeneratedClass* SkelClass = Cast<UAnimBlueprintGeneratedClass>(ChildBP->SkeletonGeneratedClass))
	{
		SkelClass->TargetSkeleton = ParentBP->TargetSkeleton;
	}

	// Inherit parent asset overrides and compile if needed
	if (ChildBP->ParentAssetOverrides.Num() > 0)
	{
		FKismetEditorUtilities::CompileBlueprint(ChildBP);
	}

	Package->SetIsExternallyReferenceable(true);
	Package->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(ChildBP);

	TArray<FString> Warnings;
	FString SaveError;
	if (!ClaireonAssetUtils::SaveAsset(ChildBP, SaveError))
	{
		Warnings.Add(FString::Printf(TEXT("Child AnimBP created but save failed: %s"), *SaveError));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), ChildBP->GetPathName());
	Result->SetStringField(TEXT("asset_name"), ChildBP->GetName());
	Result->SetStringField(TEXT("parent_class"), ParentClass->GetName());
	Result->SetStringField(TEXT("parent_animgraph"), ParentBP->GetPathName());
	Result->SetStringField(TEXT("skeleton"), ParentBP->TargetSkeleton ? ParentBP->TargetSkeleton->GetPathName() : TEXT("None"));

	TArray<ClaireonAnimGraphHelpers::FAnimGraphInfo> Graphs = ClaireonAnimGraphHelpers::CollectAllGraphs(ChildBP);
	Result->SetNumberField(TEXT("graph_count"), Graphs.Num());

	FToolResult ToolResult = MakeSuccessResult(Result, FString::Printf(TEXT("Created child Animation Blueprint: %s (parent: %s)"), *ChildBP->GetPathName(), *ParentBP->GetPathName()));
	ToolResult.Warnings = Warnings;
	return ToolResult;
}

// ============================================================================
// ClaireonAnimGraphTool_Duplicate
// ============================================================================

FString ClaireonAnimGraphTool_Duplicate::GetName() const
{
	return TEXT("claireon.animgraph_duplicate");
}

FString ClaireonAnimGraphTool_Duplicate::GetDescription() const
{
	return TEXT("Duplicate an existing Animation Blueprint to a new asset path.");
}

TSharedPtr<FJsonObject> ClaireonAnimGraphTool_Duplicate::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("source_path"), TEXT("Path to the source Animation Blueprint to duplicate"), true);
	S.AddString(TEXT("dest_path"), TEXT("Destination path for the duplicated AnimBP"), true);
	return S.Build();
}

FToolResult ClaireonAnimGraphTool_Duplicate::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SourcePath;
	if (!Arguments->TryGetStringField(TEXT("source_path"), SourcePath))
	{
		return MakeErrorResult(TEXT("Missing required field: source_path"));
	}

	FString DestPath;
	if (!Arguments->TryGetStringField(TEXT("dest_path"), DestPath))
	{
		return MakeErrorResult(TEXT("Missing required field: dest_path"));
	}

	// Resolve source
	auto SourceResolve = ClaireonPathResolver::Resolve(SourcePath);
	if (!SourceResolve.bSuccess)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to resolve source path: %s"), *SourceResolve.Error));
	}

	FString ValidationError;
	if (!ClaireonBlueprintHelpers::ValidateAssetPath(DestPath, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	// Load source to verify it's an AnimBP
	FString LoadError;
	UAnimBlueprint* SourceBP = ClaireonAnimGraphHelpers::LoadAnimBlueprint(SourceResolve.ResolvedPath.Path, LoadError);
	if (!SourceBP)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load source AnimBP: %s"), *LoadError));
	}

	// Extract destination package and name
	FString DestPackage = DestPath;
	FString DestName;
	if (DestPath.Contains(TEXT(".")))
	{
		DestPath.Split(TEXT("."), &DestPackage, &DestName);
	}
	else
	{
		int32 LastSlash;
		if (DestPackage.FindLastChar('/', LastSlash))
		{
			DestName = DestPackage.Mid(LastSlash + 1);
		}
		else
		{
			DestName = TEXT("DuplicatedAnimBlueprint");
		}
	}

	// Delete existing file at destination
	FString DestFileName = FPackageName::LongPackageNameToFilename(DestPackage, FPackageName::GetAssetPackageExtension());
	if (FPaths::FileExists(DestFileName))
	{
		IFileManager::Get().Delete(*DestFileName, false, true);
	}

	// Duplicate the asset
	UPackage* DestPkg = CreatePackage(*DestPackage);
	if (!DestPkg)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create destination package: %s"), *DestPackage));
	}

	UObject* DuplicatedObj = StaticDuplicateObject(SourceBP, DestPkg, FName(*DestName));
	UAnimBlueprint* DuplicatedBP = Cast<UAnimBlueprint>(DuplicatedObj);
	if (!DuplicatedBP)
	{
		return MakeErrorResult(TEXT("Failed to duplicate Animation Blueprint"));
	}

	DuplicatedBP->SetFlags(RF_Public | RF_Standalone);
	DestPkg->SetIsExternallyReferenceable(true);
	DestPkg->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(DuplicatedBP);

	TArray<FString> Warnings;
	FString SaveError;
	if (!ClaireonAssetUtils::SaveAsset(DuplicatedBP, SaveError))
	{
		Warnings.Add(FString::Printf(TEXT("Duplicate created but save failed: %s"), *SaveError));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("asset_path"), DuplicatedBP->GetPathName());
	Result->SetStringField(TEXT("asset_name"), DuplicatedBP->GetName());
	Result->SetStringField(TEXT("source_path"), SourceBP->GetPathName());

	FToolResult ToolResult = MakeSuccessResult(Result, FString::Printf(TEXT("Duplicated %s -> %s"), *SourceBP->GetPathName(), *DuplicatedBP->GetPathName()));
	ToolResult.Warnings = Warnings;
	return ToolResult;
}

#undef LOCTEXT_NAMESPACE
