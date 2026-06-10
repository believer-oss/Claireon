// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundMixTool_Create.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "ClaireonSessionManager.h"

#include "Sound/SoundMix.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundMixTool_Create::GetCategory() const { return TEXT("soundmix"); }
FString FClaireonSoundMixTool_Create::GetOperation() const { return TEXT("create"); }

FString FClaireonSoundMixTool_Create::GetDescription() const
{
	return TEXT("Create a new USoundMix asset at the given /Game/ content path. Non-session, "
				"immediate operation; no session_id is needed. Errors if an asset already exists at "
				"that path. Follow with soundmix.set_envelope or soundmix.add_class_adjuster to "
				"populate it; the newly-created asset is registered with the asset registry.");
}

TSharedPtr<FJsonObject> FClaireonSoundMixTool_Create::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Destination path for the new USoundMix (must start with /Game/)"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundMixTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	UObject* NewAsset = NewObject<UObject>(Package, USoundMix::StaticClass(), *ObjectName,
		RF_Public | RF_Standalone | RF_Transactional | RF_LoadCompleted);
	if (!NewAsset)
	{
		return MakeErrorResult(TEXT("NewObject failed"));
	}
	FAssetRegistryModule::AssetCreated(NewAsset);
	Package->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
	Data->SetStringField(TEXT("kind"), TEXT("sound_mix"));
	return MakeSuccessResult(Data, FString::Printf(TEXT("Created SoundMix at %s"), *NewAsset->GetPathName()));
}
