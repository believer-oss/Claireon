// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCameraAssetTool_Save.h"

#include "ClaireonSessionManager.h"
#include "Core/CameraAsset.h"
#include "Core/CameraBuildLog.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "FileHelpers.h"
#include "Logging/TokenizedMessage.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "Tools/ClaireonCameraAssetHelpers.h"
#include "UObject/Package.h"

FString FClaireonCameraAssetTool_Save::GetOperation() const { return TEXT("save"); }

FString FClaireonCameraAssetTool_Save::GetDescription() const
{
	return TEXT("Save a UCameraAsset to disk after running BuildCamera validation; returns the captured build log.");
}

TSharedPtr<FJsonObject> FClaireonCameraAssetTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("/Game/ path of the camera asset to save"), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonCameraAssetTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	// Pre-run BuildCamera with captured log. PreSave's parameterless BuildCamera()
	// overload discards diagnostics; we need to capture them before SavePackages
	// triggers the discard-overload PreSave path. The dual call is intentional —
	// PreSave's rebuild against the same in-memory state is largely idempotent.
	UE::Cameras::FCameraBuildLog Log;
	Asset->BuildCamera(Log);

	int32 ErrorCount = 0;
	for (const UE::Cameras::FCameraBuildLogMessage& M : Log.GetMessages())
	{
		if (M.Severity == EMessageSeverity::Error) { ++ErrorCount; }
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	const TSharedPtr<FJsonObject> BuildLogJson = ClaireonCameraAssetHelpers::BuildLogToJson(Log);
	const TArray<TSharedPtr<FJsonValue>>* MessagesPtr = nullptr;
	if (BuildLogJson.IsValid() && BuildLogJson->TryGetArrayField(TEXT("messages"), MessagesPtr) && MessagesPtr)
	{
		Data->SetArrayField(TEXT("build_log"), *MessagesPtr);
	}
	else
	{
		Data->SetArrayField(TEXT("build_log"), TArray<TSharedPtr<FJsonValue>>());
	}
	Data->SetNumberField(TEXT("error_count"), ErrorCount);

	// Save bytes regardless of build errors so callers can iterate on partially-
	// constructed assets (e.g. an asset without a UCameraDirector while the
	// authoring script is mid-construction). Build errors are surfaced via
	// build_log + error_count; callers that want strict gating should call
	// camera_asset_compile first.

	UPackage* Package = Asset->GetPackage();
	if (!Package)
	{
		Data->SetBoolField(TEXT("success"), false);
		Data->SetStringField(TEXT("error"), TEXT("Asset has no package"));
		return MakeSuccessResult(Data, TEXT("save failed: no package"));
	}

	TArray<UPackage*> Packages;
	Packages.Add(Package);
	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty=*/true);

	Data->SetBoolField(TEXT("success"), bSaved);
	if (!bSaved)
	{
		Data->SetStringField(TEXT("error"), TEXT("SavePackages returned false"));
	}
	return MakeSuccessResult(Data, bSaved ? TEXT("saved") : TEXT("save failed"));
}
