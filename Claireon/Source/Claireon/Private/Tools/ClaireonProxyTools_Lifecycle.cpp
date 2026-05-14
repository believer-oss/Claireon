// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonProxyTools_Lifecycle.h"
#include "Tools/ClaireonProxyTableHelpers.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/ClaireonAnimEditToolBase.h"
#include "ClaireonSessionManager.h"
#include "ProxyTable.h"
#include "ProxyAsset.h"
#include "ChooserPropertyAccess.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"

// ============================================================================
// proxytable_create
// ============================================================================

FString ClaireonTool_ProxyTableCreate::GetCategory() const { return TEXT("proxytable"); }
FString ClaireonTool_ProxyTableCreate::GetOperation() const { return TEXT("create"); }

FString ClaireonTool_ProxyTableCreate::GetDescription() const
{
	return TEXT("Create a new empty ProxyTable asset.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyTableCreate::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("path"), TEXT("Target asset path (e.g. /Game/Data/ProxyTables/PT_NewTable)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyTableCreate::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	// Match the flags from UProxyTableFactory::FactoryCreateNew
	UProxyTable* ProxyTable = NewObject<UProxyTable>(Package, FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional | RF_LoadCompleted);

	if (!ProxyTable)
	{
		return MakeErrorResult(TEXT("Failed to create ProxyTable asset"));
	}

	FAssetRegistryModule::AssetCreated(ProxyTable);

	if (!ClaireonChooserHelpers::SaveNewAsset(ProxyTable, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyTable->GetPathName());
	Data->SetStringField(TEXT("asset_name"), AssetName);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Created ProxyTable '%s'"), *AssetName));
}

// ============================================================================
// proxytable_duplicate
// ============================================================================

FString ClaireonTool_ProxyTableDuplicate::GetCategory() const { return TEXT("proxytable"); }
FString ClaireonTool_ProxyTableDuplicate::GetOperation() const { return TEXT("duplicate"); }

FString ClaireonTool_ProxyTableDuplicate::GetDescription() const
{
	return TEXT("Duplicate an existing ProxyTable asset to a new path.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyTableDuplicate::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("source_path"), TEXT("Path to the source ProxyTable"), true);
	S.AddString(TEXT("dest_path"), TEXT("Destination asset path"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyTableDuplicate::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	UProxyTable* Source = ClaireonProxyTableHelpers::LoadProxyTableAsset(SourcePath, Error);
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
		return MakeErrorResult(FString::Printf(TEXT("Failed to duplicate ProxyTable to '%s'"), *DestCanon));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source_path"), Source->GetPathName());
	Data->SetStringField(TEXT("dest_path"), NewAsset->GetPathName());
	Data->SetStringField(TEXT("asset_name"), NewAsset->GetName());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Duplicated ProxyTable to '%s'"), *NewAsset->GetName()));
}

// ============================================================================
// proxyasset_create
// ============================================================================

FString ClaireonTool_ProxyAssetCreate::GetCategory() const { return TEXT("proxyasset"); }
FString ClaireonTool_ProxyAssetCreate::GetOperation() const { return TEXT("create"); }

FString ClaireonTool_ProxyAssetCreate::GetDescription() const
{
	return TEXT("Create a new ProxyAsset. Optionally set the type (the class of objects this proxy maps to).");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyAssetCreate::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("path"), TEXT("Target asset path (e.g. /Game/Data/Proxies/PA_NewProxy)"), true);
	S.AddString(TEXT("type"), TEXT("Class path for the proxy's Type (e.g. /Script/Engine.AnimSequence)"));
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyAssetCreate::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	// Match the flags from UProxyAssetFactory::FactoryCreateNew
	UProxyAsset* ProxyAsset = NewObject<UProxyAsset>(Package, FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional | RF_LoadCompleted);

	if (!ProxyAsset)
	{
		return MakeErrorResult(TEXT("Failed to create ProxyAsset"));
	}

	// Generate a unique GUID (the factory normally does this)
	ProxyAsset->Guid = FGuid::NewGuid();

	// Set type if provided
	FString TypeStr;
	if (Arguments->TryGetStringField(TEXT("type"), TypeStr) && !TypeStr.IsEmpty())
	{
		UClass* TypeClass = FindObject<UClass>(nullptr, *TypeStr);
		if (!TypeClass)
		{
			TypeClass = LoadObject<UClass>(nullptr, *TypeStr);
		}
		if (TypeClass)
		{
			ProxyAsset->Type = TypeClass;
		}
	}

	FAssetRegistryModule::AssetCreated(ProxyAsset);

	if (!ClaireonChooserHelpers::SaveNewAsset(ProxyAsset, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), ProxyAsset->GetPathName());
	Data->SetStringField(TEXT("asset_name"), AssetName);
	Data->SetStringField(TEXT("guid"), ProxyAsset->Guid.ToString());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Created ProxyAsset '%s'"), *AssetName));
}

// ============================================================================
// proxyasset_duplicate
// ============================================================================

FString ClaireonTool_ProxyAssetDuplicate::GetCategory() const { return TEXT("proxyasset"); }
FString ClaireonTool_ProxyAssetDuplicate::GetOperation() const { return TEXT("duplicate"); }

FString ClaireonTool_ProxyAssetDuplicate::GetDescription() const
{
	return TEXT("Duplicate an existing ProxyAsset to a new path.");
}

TSharedPtr<FJsonObject> ClaireonTool_ProxyAssetDuplicate::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("source_path"), TEXT("Path to the source ProxyAsset"), true);
	S.AddString(TEXT("dest_path"), TEXT("Destination asset path"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ProxyAssetDuplicate::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	UProxyAsset* Source = ClaireonProxyTableHelpers::LoadProxyAsset(SourcePath, Error);
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
		return MakeErrorResult(FString::Printf(TEXT("Failed to duplicate ProxyAsset to '%s'"), *DestCanon));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("source_path"), Source->GetPathName());
	Data->SetStringField(TEXT("dest_path"), NewAsset->GetPathName());
	Data->SetStringField(TEXT("asset_name"), NewAsset->GetName());

	return MakeSuccessResult(Data, FString::Printf(TEXT("Duplicated ProxyAsset to '%s'"), *NewAsset->GetName()));
}
