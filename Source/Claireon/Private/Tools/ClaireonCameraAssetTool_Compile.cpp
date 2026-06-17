// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_Compile.h"

#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Logging/TokenizedMessage.h"
#include "Misc/EngineVersionComparison.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonCameraAssetHelpers.h"

#if WITH_GAMEPLAY_CAMERAS

#include "Core/CameraAsset.h"
#if UE_VERSION_OLDER_THAN(5, 6, 0)
#include "Core/CameraBuildLog.h"
#else
#include "Build/CameraBuildLog.h"
#endif

FString FClaireonCameraAssetTool_Compile::GetOperation() const { return TEXT("compile"); }

FString FClaireonCameraAssetTool_Compile::GetDescription() const
{
	return TEXT("Run UCameraAsset::BuildCamera against in-memory state without persisting; returns build diagnostics.");
}

TSharedPtr<FJsonObject> FClaireonCameraAssetTool_Compile::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("/Game/ path of the camera asset to compile"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonCameraAssetTool_Compile::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	UE::Cameras::FCameraBuildLog Log;
	Asset->BuildCamera(Log);

	bool bHasErrors = false;
	for (const UE::Cameras::FCameraBuildLogMessage& M : Log.GetMessages())
	{
		if (M.Severity == EMessageSeverity::Error)
		{
			bHasErrors = true;
			break;
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ok"), !bHasErrors);

	// BuildLogToJson returns {messages: [...]}; lift the array onto our 'errors' field.
	const TSharedPtr<FJsonObject> BuildLogJson = ClaireonCameraAssetHelpers::BuildLogToJson(Log);
	const TArray<TSharedPtr<FJsonValue>>* MessagesPtr = nullptr;
	if (BuildLogJson.IsValid() && BuildLogJson->TryGetArrayField(TEXT("messages"), MessagesPtr) && MessagesPtr)
	{
		Data->SetArrayField(TEXT("errors"), *MessagesPtr);
	}
	else
	{
		Data->SetArrayField(TEXT("errors"), TArray<TSharedPtr<FJsonValue>>());
	}

	return MakeSuccessResult(Data, bHasErrors ? TEXT("compile failed") : TEXT("compile ok"));
}

#endif // WITH_GAMEPLAY_CAMERAS
