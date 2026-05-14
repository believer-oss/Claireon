// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMetaSoundTool_Create.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "ClaireonSessionManager.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"

#if __has_include("MetasoundSource.h")
#include "MetasoundSource.h"
#endif

FString FClaireonMetaSoundTool_Create::GetCategory() const { return TEXT("metasound"); }
FString FClaireonMetaSoundTool_Create::GetOperation() const { return TEXT("create"); }

FString FClaireonMetaSoundTool_Create::GetDescription() const
{
	return TEXT("Create an empty UMetaSoundSource at the given /Game/ path. Errors if an asset already exists.");
}

TSharedPtr<FJsonObject> FClaireonMetaSoundTool_Create::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Destination path for the new UMetaSoundSource (must start with /Game/)"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonMetaSoundTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if !__has_include("MetasoundSource.h")
	return MakeErrorResult(TEXT("MetaSound module not available on this engine branch"));
#else
	if (!Arguments.IsValid()) return MakeErrorResult(TEXT("Arguments object missing"));
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
	UObject* NewAsset = NewObject<UObject>(Package, UMetaSoundSource::StaticClass(), *ObjectName,
		RF_Public | RF_Standalone | RF_Transactional | RF_LoadCompleted);
	if (!NewAsset) return MakeErrorResult(TEXT("NewObject failed"));
	FAssetRegistryModule::AssetCreated(NewAsset);
	Package->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
	Data->SetStringField(TEXT("kind"), TEXT("metasound_source"));
	return MakeSuccessResult(Data, FString::Printf(TEXT("Created MetaSoundSource at %s"), *NewAsset->GetPathName()));
#endif
}
