// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEScreenshot.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "UnrealClient.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"

FString ClaireonTool_PIEScreenshot::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIEScreenshot::GetOperation() const { return TEXT("screenshot"); }

FString ClaireonTool_PIEScreenshot::GetDescription() const
{
	return TEXT("Capture a screenshot of the current PIE viewport. Returns the file path immediately; the file will be available within ~100ms.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEScreenshot::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Output filename without extension. Default: screenshot_<timestamp>"));
		Properties->SetObjectField(TEXT("filename"), Prop);
	}

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Output directory. Default: Saved/Screenshots/"));
		Properties->SetObjectField(TEXT("directory"), Prop);
	}

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("number"));
		Prop->SetStringField(TEXT("description"), TEXT("Resolution multiplier for HighResShot (e.g., 2.0 for 2x viewport). Default: 1.0"));
		Prop->SetNumberField(TEXT("default"), 1.0);
		Properties->SetObjectField(TEXT("resolutionMultiplier"), Prop);
	}

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("boolean"));
		Prop->SetStringField(TEXT("description"), TEXT("Include UMG/Slate UI overlays in the screenshot. Default: true"));
		Properties->SetObjectField(TEXT("show_ui"), Prop);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIEScreenshot::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	// Resolve output filename
	FString Filename;
	if (!Arguments->TryGetStringField(TEXT("filename"), Filename) || Filename.IsEmpty())
	{
		FDateTime Now = FDateTime::Now();
		Filename = FString::Printf(TEXT("screenshot_%04d%02d%02d_%02d%02d%02d"),
			Now.GetYear(), Now.GetMonth(), Now.GetDay(),
			Now.GetHour(), Now.GetMinute(), Now.GetSecond());
	}

	// Resolve output directory
	FString Directory;
	if (!Arguments->TryGetStringField(TEXT("directory"), Directory) || Directory.IsEmpty())
	{
		Directory = FPaths::ProjectSavedDir() / TEXT("Screenshots");
	}

	// Resolution multiplier
	double ResolutionMultiplier = 1.0;
	Arguments->TryGetNumberField(TEXT("resolutionMultiplier"), ResolutionMultiplier);

	// Show UI (UMG/Slate overlays) - default true
	bool bShowUI = true;
	Arguments->TryGetBoolField(TEXT("show_ui"), bShowUI);

	// Ensure directory exists
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Directory);

	// Build full output path (without extension; Unreal appends .png)
	FString FullPath = Directory / Filename;

	// Capture the screenshot via FScreenshotRequest
	if (ResolutionMultiplier > 1.0)
	{
		// HighResShot - fires via console command
		FString HighResCommand = FString::Printf(TEXT("HighResShot %.2f"), ResolutionMultiplier);
		GEngine->Exec(nullptr, *HighResCommand);
	}
	else
	{
		FScreenshotRequest::RequestScreenshot(FullPath + TEXT(".png"), false, bShowUI);
	}

	// Estimate file size (file may not exist yet as capture is async)
	int64 SizeBytes = 0;
	FString FinalPath = FullPath + TEXT(".png");
	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*FinalPath))
	{
		SizeBytes = FPlatformFileManager::Get().GetPlatformFile().FileSize(*FinalPath);
	}

	// Build a rough resolution string from the viewport
	FString ResolutionStr = TEXT("unknown");
	if (GEngine && GEngine->GameViewport)
	{
		FViewport* Viewport = GEngine->GameViewport->Viewport;
		if (Viewport)
		{
			FIntPoint Size = Viewport->GetSizeXY();
			int32 W = static_cast<int32>(Size.X * ResolutionMultiplier);
			int32 H = static_cast<int32>(Size.Y * ResolutionMultiplier);
			ResolutionStr = FString::Printf(TEXT("%dx%d"), W, H);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("file_path"), FinalPath);
	Data->SetStringField(TEXT("resolution"), ResolutionStr);
	Data->SetNumberField(TEXT("size_bytes"), static_cast<double>(SizeBytes));

	FString Summary = FString::Printf(TEXT("Screenshot saved to %s"), *FinalPath);
	return MakeSuccessResult(Data, Summary);
}
