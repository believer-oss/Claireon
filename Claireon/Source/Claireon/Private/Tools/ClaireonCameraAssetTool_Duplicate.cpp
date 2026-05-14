// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_Duplicate.h"
#include "ClaireonSessionManager.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Core/CameraAsset.h"
#include "Dom/JsonObject.h"
#include "EditorAssetLibrary.h"

FString FClaireonCameraAssetTool_Duplicate::GetOperation() const { return TEXT("duplicate"); }

FString FClaireonCameraAssetTool_Duplicate::GetDescription() const
{
	return TEXT("Duplicate an existing UCameraAsset to a new /Game/ path. Errors if source missing or destination exists.");
}

TSharedPtr<FJsonObject> FClaireonCameraAssetTool_Duplicate::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("source_path"), TEXT("Source /Game/ path of the camera asset to duplicate"), true);
	S.AddString(TEXT("dest_path"), TEXT("Destination /Game/ path for the new camera asset"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonCameraAssetTool_Duplicate::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	FString Source;
	if (!Arguments->TryGetStringField(TEXT("source_path"), Source) || Source.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: source_path"));
	}
	FString Dest;
	if (!Arguments->TryGetStringField(TEXT("dest_path"), Dest) || Dest.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: dest_path"));
	}

	const FString CanonSrc = FClaireonSessionManager::CanonicalizePath(Source);
	if (CanonSrc.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid source_path (must start with /Game/)"));
	}
	const FString CanonDest = FClaireonSessionManager::CanonicalizePath(Dest);
	if (CanonDest.IsEmpty())
	{
		return MakeErrorResult(TEXT("Invalid dest_path (must start with /Game/)"));
	}

	if (!LoadObject<UCameraAsset>(nullptr, *CanonSrc))
	{
		return MakeErrorResult(FString::Printf(TEXT("Source camera asset not found: %s"), *CanonSrc));
	}
	if (LoadObject<UObject>(nullptr, *CanonDest))
	{
		return MakeErrorResult(FString::Printf(TEXT("Destination asset already exists: %s"), *CanonDest));
	}

	UObject* Dup = UEditorAssetLibrary::DuplicateAsset(CanonSrc, CanonDest);
	if (!Dup)
	{
		return MakeErrorResult(TEXT("UEditorAssetLibrary::DuplicateAsset returned null"));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("dest_path"), Dup->GetPathName());
	return MakeSuccessResult(Data, FString::Printf(TEXT("Duplicated %s to %s"), *CanonSrc, *Dup->GetPathName()));
}
