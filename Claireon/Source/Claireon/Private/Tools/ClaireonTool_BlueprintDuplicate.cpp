// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintDuplicate.h"

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonPathResolver.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"

#include "Editor.h"
#include "Engine/AssetManagerTypes.h"
#include "Engine/Blueprint.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#include "Misc/PackageName.h"
#include "Misc/Paths.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "UObject/PropertyPortFlags.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_BlueprintDuplicate::GetName() const
{
	return TEXT("claireon.blueprint_duplicate");
}

FString ClaireonTool_BlueprintDuplicate::GetDescription() const
{
	return TEXT(
		"Duplicate a Blueprint asset (Blueprint, AnimBlueprint, WidgetBlueprint, or any "
		"UBlueprint-derived class) to a new path. Resolves source and destination via "
		"ClaireonPathResolver, validates the source is in the Blueprint family, performs "
		"IAssetTools::DuplicateAsset, notifies AssetRegistry::AssetCreated, and saves "
		"the duplicate to disk. When rename_dependencies=true, rewrites the duplicate's "
		"internal soft references (FSoftObjectPath, FSoftClassPath, TSoftObjectPtr<T>, "
		"FPrimaryAssetId/FPrimaryAssetType) that point back at the source package so they "
		"point at the new package. The destination folder is created automatically if it "
		"does not exist. The source may be open in the editor; the duplicate is not "
		"opened automatically."
	);
}

TSharedPtr<FJsonObject> ClaireonTool_BlueprintDuplicate::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> SourcePathProp = MakeShared<FJsonObject>();
	SourcePathProp->SetStringField(TEXT("type"), TEXT("string"));
	SourcePathProp->SetStringField(TEXT("description"),
		TEXT("Absolute or Claireon-resolvable path to the source Blueprint (e.g. "
			 "/Game/Characters/BP_Hero)."));
	Properties->SetObjectField(TEXT("source_path"), SourcePathProp);

	TSharedPtr<FJsonObject> DestPathProp = MakeShared<FJsonObject>();
	DestPathProp->SetStringField(TEXT("type"), TEXT("string"));
	DestPathProp->SetStringField(TEXT("description"),
		TEXT("Absolute or Claireon-resolvable path for the destination Blueprint (e.g. "
			 "/Game/Sandbox/BP_Hero_Clone). The destination folder is created automatically "
			 "if it does not already exist."));
	Properties->SetObjectField(TEXT("dest_path"), DestPathProp);

	TSharedPtr<FJsonObject> RenameDepsProp = MakeShared<FJsonObject>();
	RenameDepsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	RenameDepsProp->SetStringField(TEXT("description"),
		TEXT("When true, rewrite the duplicate's internal self-references (FSoftObjectPath, "
			 "FSoftClassPath, TSoftObjectPtr<T>, FPrimaryAssetId/FPrimaryAssetType) that "
			 "point back at the source package so they point at the new package. Default: "
			 "false. FName-typed path-holder variables are intentionally not rewritten."));
	Properties->SetObjectField(TEXT("rename_dependencies"), RenameDepsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("source_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("dest_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

// -----------------------------------------------------------------------------
// D5 reference rewrite helper
// -----------------------------------------------------------------------------

static bool TryRewriteSoftObjectPath(FSoftObjectPath& InOutPath, const FString& SourcePackagePath, const FString& DestPackagePath)
{
	const FString PackageName = InOutPath.GetLongPackageName();
	if (PackageName != SourcePackagePath)
	{
		return false;
	}

	// Preserve the sub-path (asset name + optional subobject path).
	const FString AssetName = InOutPath.GetAssetName();
	const FString SubPath = InOutPath.GetSubPathString();

	// Rebuild the new asset path. If the asset name equals the source's
	// short name, substitute the destination's short name; otherwise keep the
	// asset name as-is (unusual, but defensive).
	const FString SourceShortName = FPackageName::GetShortName(SourcePackagePath);
	const FString DestShortName = FPackageName::GetShortName(DestPackagePath);

	FString NewAssetName = AssetName;
	if (AssetName == SourceShortName)
	{
		NewAssetName = DestShortName;
	}

	FString NewPath = DestPackagePath;
	NewPath.Append(TEXT("."));
	NewPath.Append(NewAssetName);
	if (!SubPath.IsEmpty())
	{
		NewPath.Append(TEXT(":"));
		NewPath.Append(SubPath);
	}

	InOutPath = FSoftObjectPath(NewPath);
	return true;
}

static void RewriteSelfReferences(UObject* NewAsset, const FString& SourcePackagePath, const FString& DestPackagePath)
{
	if (!NewAsset)
	{
		return;
	}

	const FString SourceShortName = FPackageName::GetShortName(SourcePackagePath);
	const FString DestShortName = FPackageName::GetShortName(DestPackagePath);

	bool bModified = false;

	// (1) + (2) + (3): FSoftObjectProperty covers FSoftObjectPath, FSoftClassPath, and
	// TSoftObjectPtr<T> wrappers (which are stored under FSoftObjectProperty reflection).
	for (TPropertyValueIterator<FSoftObjectProperty> It(NewAsset->GetClass(), NewAsset); It; ++It)
	{
		void* ValuePtr = const_cast<void*>(It.Value());
		FSoftObjectPtr* SoftPtr = reinterpret_cast<FSoftObjectPtr*>(ValuePtr);
		FSoftObjectPath CurrentPath = SoftPtr->ToSoftObjectPath();
		if (TryRewriteSoftObjectPath(CurrentPath, SourcePackagePath, DestPackagePath))
		{
			*SoftPtr = FSoftObjectPtr(CurrentPath);
			bModified = true;
		}
	}

	// (4) FPrimaryAssetId / FPrimaryAssetType: iterate struct properties.
	UScriptStruct* PrimaryAssetIdStruct = TBaseStructure<FPrimaryAssetId>::Get();
	UScriptStruct* PrimaryAssetTypeStruct = TBaseStructure<FPrimaryAssetType>::Get();

	for (TPropertyValueIterator<FStructProperty> It(NewAsset->GetClass(), NewAsset); It; ++It)
	{
		const FStructProperty* StructProp = It.Key();
		if (!StructProp || !StructProp->Struct)
		{
			continue;
		}

		void* ValuePtr = const_cast<void*>(It.Value());

		if (StructProp->Struct == PrimaryAssetIdStruct)
		{
			FPrimaryAssetId* Id = reinterpret_cast<FPrimaryAssetId*>(ValuePtr);
			if (Id->PrimaryAssetName == FName(*SourceShortName))
			{
				Id->PrimaryAssetName = FName(*DestShortName);
				bModified = true;
			}
		}
		else if (StructProp->Struct == PrimaryAssetTypeStruct)
		{
			// FPrimaryAssetType wraps an FName representing the type; it does
			// not encode asset paths, so nothing to rewrite here. Still covered
			// for completeness per the D5 enumeration; no-op.
		}
	}

	if (bModified)
	{
		NewAsset->MarkPackageDirty();
	}
}

IClaireonTool::FToolResult ClaireonTool_BlueprintDuplicate::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// 1. Validate Arguments
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}

	// 2. source_path
	FString SourcePath;
	if (!Arguments->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: source_path"));
	}

	// 3. dest_path
	FString DestPath;
	if (!Arguments->TryGetStringField(TEXT("dest_path"), DestPath) || DestPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: dest_path"));
	}

	// 4. rename_dependencies (optional, default false)
	bool bRenameDependencies = false;
	Arguments->TryGetBoolField(TEXT("rename_dependencies"), bRenameDependencies);

	// 5. Editor availability
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	// 6. Resolve source path
	FString SourcePackagePath;
	{
		auto SrcResolve = ClaireonPathResolver::Resolve(SourcePath);
		if (!SrcResolve.bSuccess)
		{
			return MakeErrorResult(SrcResolve.Error);
		}
		SourcePath = SrcResolve.ResolvedPath.Path;
		SourcePackagePath = SrcResolve.ResolvedPath.PackagePath;
	}

	// 7. Resolve destination path
	FString DestPackagePath;
	{
		auto DstResolve = ClaireonPathResolver::Resolve(DestPath);
		if (!DstResolve.bSuccess)
		{
			return MakeErrorResult(DstResolve.Error);
		}
		DestPath = DstResolve.ResolvedPath.Path;
		DestPackagePath = DstResolve.ResolvedPath.PackagePath;
	}

	// 8. Asset registry lookup for source
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	FAssetData SourceData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(SourcePath));
	if (!SourceData.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Source not found: %s"), *SourcePath));
	}

	// 9. D1 Branch 1 (allowlist, no load)
	const FString SourceClassName = SourceData.AssetClassPath.GetAssetName().ToString();
	const bool bBranch1 = ClaireonBlueprintHelpers::IsBlueprintAssetClass(SourceClassName);
	FString AcceptBranch;
	if (bBranch1)
	{
		AcceptBranch = TEXT("allowlist");
	}

	// 10. D1 Branch 2 (IsChildOf<UBlueprint>, requires load)
	UObject* LoadedSource = nullptr;
	bool bBranch2 = false;
	if (!bBranch1)
	{
		LoadedSource = FSoftObjectPath(SourcePath).TryLoad();
		if (LoadedSource && LoadedSource->GetClass()->IsChildOf(UBlueprint::StaticClass()))
		{
			AcceptBranch = TEXT("IsChildOf<UBlueprint>");
			bBranch2 = true;
		}
	}

	// 11. Reject if neither branch accepted
	if (!bBranch1 && !bBranch2)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Source rejected: class '%s' is not in the Blueprint allowlist {Blueprint, AnimBlueprint, WidgetBlueprint} and is not a loaded subclass of UBlueprint"),
			*SourceClassName));
	}

	// 12. Debug-log the accept branch
	UE_LOG(LogClaireon, Verbose, TEXT("Source accepted via %s"), *AcceptBranch);

	// 13. Destination-exists check
	FAssetData DestData = AssetRegistry.GetAssetByObjectPath(FSoftObjectPath(DestPath));
	if (DestData.IsValid())
	{
		const FString DestExistingClass = DestData.AssetClassPath.GetAssetName().ToString();
		return MakeErrorResult(FString::Printf(
			TEXT("Destination already exists: %s (class=%s)"),
			*DestPackagePath, *DestExistingClass));
	}

	// 14. Ensure source is loaded for DuplicateAsset
	if (bBranch1 && LoadedSource == nullptr)
	{
		LoadedSource = FSoftObjectPath(SourcePath).TryLoad();
	}
	if (!LoadedSource)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to duplicate Blueprint to %s"), *DestPath));
	}

	// 15. Compute DestFolder and DestName
	const FString DestFolder = FPackageName::GetLongPackagePath(DestPackagePath);
	const FString DestName = FPackageName::GetShortName(DestPackagePath);

	// 16. Duplicate via IAssetTools::DuplicateAsset
	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	UObject* NewAsset = AssetToolsModule.Get().DuplicateAsset(DestName, DestFolder, LoadedSource);
	if (!NewAsset)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to duplicate Blueprint to %s"), *DestPath));
	}

	// 17. Canonical AssetCreated notification (D4 - idempotent)
	FAssetRegistryModule::AssetCreated(NewAsset);

	// 18. Optional D5 reference rewrite
	if (bRenameDependencies)
	{
		RewriteSelfReferences(NewAsset, SourcePackagePath, DestPackagePath);
	}

	// 19. Save the new package
	UPackage* Package = NewAsset->GetOutermost();
	Package->MarkPackageDirty();
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	FSavePackageResultStruct SaveResult = UPackage::Save(Package, NewAsset, *PackageFileName, SaveArgs);
	if (SaveResult.Result != ESavePackageResult::Success)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to save duplicated Blueprint at %s"), *DestPath));
	}

	// 20. Build success response
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("ok"));
	Data->SetStringField(TEXT("source_path"), SourcePath);
	Data->SetStringField(TEXT("dest_path"), DestPath);
	Data->SetStringField(TEXT("asset_name"), DestName);
	Data->SetStringField(TEXT("asset_class"), SourceClassName);
	Data->SetStringField(TEXT("class_accept_branch"), AcceptBranch);
	Data->SetBoolField(TEXT("rename_dependencies"), bRenameDependencies);

	const FString Summary = FString::Printf(TEXT("Duplicated %s to %s"), *SourcePath, *DestPath);
	return MakeSuccessResult(Data, Summary);
}
