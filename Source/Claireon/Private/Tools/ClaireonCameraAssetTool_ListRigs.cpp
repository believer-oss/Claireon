// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_ListRigs.h"

#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonCameraAssetHelpers.h"

#if WITH_GAMEPLAY_CAMERAS

#include "Core/CameraAsset.h"
#include "Core/CameraNode.h"
#include "Core/CameraRigAsset.h"

FString FClaireonCameraAssetTool_ListRigs::GetOperation() const { return TEXT("list_rigs"); }

FString FClaireonCameraAssetTool_ListRigs::GetDescription() const
{
	return TEXT("List all UCameraRigAsset entries on a UCameraAsset, with rig index, name, and root node class.");
}

TSharedPtr<FJsonObject> FClaireonCameraAssetTool_ListRigs::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("/Game/ path of the camera asset"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonCameraAssetTool_ListRigs::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
		return MakeErrorResult(TEXT("Invalid asset_path (must start with /Game/)"));
	}

	UCameraAsset* Asset = LoadObject<UCameraAsset>(nullptr, *Canon);
	if (!Asset)
	{
		return MakeErrorResult(FString::Printf(TEXT("Camera asset not found: %s"), *Canon));
	}

	TArray<TSharedPtr<FJsonValue>> RigsJson;
	int32 Index = 0;
	const TArray<UCameraRigAsset*> Rigs = ClaireonCameraAssetHelpers::GetCameraRigs(Asset);
	for (UCameraRigAsset* Rig : Rigs)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetNumberField(TEXT("rig_index"), Index);
		Entry->SetStringField(TEXT("rig_name"), Rig ? Rig->GetName() : FString());

		FString RootClass;
		if (Rig && Rig->RootNode)
		{
			RootClass = Rig->RootNode->GetClass()->GetName();
		}
		Entry->SetStringField(TEXT("root_node_class"), RootClass);

		RigsJson.Add(MakeShared<FJsonValueObject>(Entry));
		++Index;
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("rigs"), RigsJson);
	Data->SetNumberField(TEXT("count"), RigsJson.Num());
	return MakeSuccessResult(Data,
		FString::Printf(TEXT("Listed %d rig(s) on %s"), RigsJson.Num(), *Asset->GetPathName()));
}

#endif // WITH_GAMEPLAY_CAMERAS
