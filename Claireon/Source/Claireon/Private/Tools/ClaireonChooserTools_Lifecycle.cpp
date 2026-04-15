// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTools_Lifecycle.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "ClaireonSessionManager.h"
#include "Chooser.h"
#include "ChooserPropertyAccess.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"

// ============================================================================
// claireon.chooser_create
// ============================================================================

FString ClaireonTool_ChooserCreate::GetName() const { return TEXT("claireon.chooser_create"); }

FString ClaireonTool_ChooserCreate::GetDescription() const
{
	return TEXT("Create a new empty ChooserTable asset. Optionally set the result type and output class.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserCreate::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("path"), TEXT("Target asset path (e.g. /Game/Data/Choosers/CHT_NewChooser)"), true);
	S.AddEnum(TEXT("result_type"), TEXT("Result type: 'ObjectResult' or 'ClassResult'"),
		{TEXT("ObjectResult"), TEXT("ClassResult")});
	S.AddString(TEXT("output_class"), TEXT("Class path for the output object type (e.g. /Script/Engine.AnimSequence)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserCreate::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Path;
	if (!Arguments->TryGetStringField(TEXT("path"), Path) || Path.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: path"));
	}

	FString CanonPath, AssetName, Error;
	if (!ClaireonChooserHelpers::ValidateNewAssetPath(Path, CanonPath, AssetName, Error))
	{
		return MakeErrorResult(Error);
	}

	UPackage* Package = CreatePackage(*CanonPath);
	// Match the flags from UChooserTableFactory::FactoryCreateNew
	UChooserTable* Chooser = NewObject<UChooserTable>(Package, FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional | RF_LoadCompleted);

	if (!Chooser)
	{
		return MakeErrorResult(TEXT("Failed to create ChooserTable asset"));
	}

	// Set result type if provided
	FString ResultTypeStr;
	if (Arguments->TryGetStringField(TEXT("result_type"), ResultTypeStr))
	{
		if (ResultTypeStr == TEXT("ClassResult"))
		{
			Chooser->ResultType = EObjectChooserResultType::ClassResult;
		}
		else
		{
			Chooser->ResultType = EObjectChooserResultType::ObjectResult;
		}
	}

	// Set output class if provided
	FString OutputClassStr;
	if (Arguments->TryGetStringField(TEXT("output_class"), OutputClassStr) && !OutputClassStr.IsEmpty())
	{
		UClass* OutputClass = FindObject<UClass>(nullptr, *OutputClassStr);
		if (!OutputClass)
		{
			OutputClass = LoadObject<UClass>(nullptr, *OutputClassStr);
		}
		if (OutputClass)
		{
			Chooser->OutputObjectType = OutputClass;
		}
	}

	FAssetRegistryModule::AssetCreated(Chooser);

	if (!ClaireonChooserHelpers::SaveNewAsset(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetStringField(TEXT("asset_name"), AssetName);
	Data->SetStringField(TEXT("result_type"), ClaireonChooserHelpers::ResultTypeToString(static_cast<uint8>(Chooser->ResultType)));

	return MakeSuccessResult(Data, FString::Printf(TEXT("Created ChooserTable '%s'"), *AssetName));
}

// ============================================================================
// claireon.chooser_duplicate
// ============================================================================

FString ClaireonTool_ChooserDuplicate::GetName() const { return TEXT("claireon.chooser_duplicate"); }

FString ClaireonTool_ChooserDuplicate::GetDescription() const
{
	return TEXT("Duplicate an existing ChooserTable asset to a new path.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserDuplicate::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("source_path"), TEXT("Path to the source ChooserTable"), true);
	S.AddString(TEXT("dest_path"), TEXT("Destination asset path"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserDuplicate::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SourcePath, DestPath;
	if (!Arguments->TryGetStringField(TEXT("source_path"), SourcePath) || SourcePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: source_path"));
	}
	if (!Arguments->TryGetStringField(TEXT("dest_path"), DestPath) || DestPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: dest_path"));
	}

	FString Error;
	UChooserTable* Source = ClaireonChooserHelpers::LoadChooserTableAsset(SourcePath, Error);
	if (!Source)
	{
		return MakeErrorResult(Error);
	}

	FString DestCanon = FClaireonSessionManager::CanonicalizePath(DestPath);
	if (DestCanon.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid destination path. Must start with /Game/."));
	}

	FString DestName = FPackageName::GetShortName(DestCanon);
	FString DestFolder = FPackageName::GetLongPackagePath(DestCanon);

	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	UObject* NewAsset = AssetTools.DuplicateAsset(DestName, DestFolder, Source);
	if (!NewAsset)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to duplicate ChooserTable to '%s'"), *DestCanon));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source_path"), Source->GetPathName());
	Data->SetStringField(TEXT("dest_path"), NewAsset->GetPathName());
	Data->SetStringField(TEXT("asset_name"), NewAsset->GetName());

	FToolResult Result = MakeSuccessResult(Data, FString::Printf(TEXT("Duplicated ChooserTable to '%s'"), *NewAsset->GetName()));

#if WITH_EDITORONLY_DATA
	// Warn about nested chooser cross-package reference issues
	if (Source->NestedChoosers.Num() > 0)
	{
		Result.Warnings.Add(FString::Printf(
			TEXT("Source has %d nested choosers. Multi-level nested choosers may have cross-package references "
			"that prevent saving. If you encounter save errors, consider creating the chooser from scratch "
			"using chooser_create + chooser_add_column + chooser_add_row instead of duplicating."),
			Source->NestedChoosers.Num()));
	}
#endif

	return Result;
}
