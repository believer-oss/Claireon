// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SlateScreenshot.h"
#include "Dom/JsonObject.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SWindow.h"
#include "ImageUtils.h"
#include "ImageCore.h"
#include "UnrealClient.h"
#include "LevelEditor.h"
#include "SLevelViewport.h"
#include "Modules/ModuleManager.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericWindow.h"
#include "Rendering/SlateRenderer.h"
#include "Misc/ScopeExit.h"
#include "Templates/Function.h"

namespace ClaireonSlateScreenshotPrivate
{
	static bool IsCapturableState(bool bNativeValid, bool bHasHandle, bool bSlateVisible,
		bool bNativeVisible, bool bMinimized, const FVector2D& Size)
	{
		return bNativeValid && bHasHandle && bSlateVisible && bNativeVisible && !bMinimized
			&& FMath::IsFinite(Size.X) && FMath::IsFinite(Size.Y)
			&& Size.X >= 1 && Size.Y >= 1 && Size.X * Size.Y <= MAX_int32;
	}

	static bool IsCapturable(const TSharedPtr<SWindow>& Window)
	{
		if (!Window) { return false; }
		const TSharedPtr<FGenericWindow> Native = Window->GetNativeWindow();
		return IsCapturableState(Native.IsValid(), Native && Native->GetOSWindowHandle() != nullptr,
			Window->IsVisible(), Native && Native->IsVisible(), Window->IsWindowMinimized(), Window->GetSizeInScreen());
	}

	// The callbacks isolate request lifetime from native/RHI setup for headless regression tests.
	static bool CaptureAndRelease(TFunctionRef<bool()> Capture,
		TFunctionRef<void(TArray<FColor>&)> Detach, TFunctionRef<void()> Flush)
	{
		static TArray<FColor> DetachedBuffer;
		ON_SCOPE_EXIT
		{
			// Drain draws reading ScreenshotState before changing it on the game thread.
			Flush();
			// UE 5.5 retains the request when its window was not drawn. Null window clears its viewport.
			Detach(DetachedBuffer);
			Flush();
		};
		return Capture();
	}
}

FString ClaireonTool_SlateScreenshot::GetCategory() const { return TEXT("slate"); }
FString ClaireonTool_SlateScreenshot::GetOperation() const { return TEXT("screenshot"); }

FString ClaireonTool_SlateScreenshot::GetDescription() const
{
	return TEXT("Capture a Slate surface to a PNG and return its on-disk path. target=\"window\" (default) captures "
		"a top-level Slate window (asset-editor UI) selected by the \"window\" title substring or the active "
		"window; target=\"level_viewport\" captures the level viewport's rendered 3D scene. Writes the PNG "
		"synchronously, so the file exists on return. Non-session, immediate.");
}

TSharedPtr<FJsonObject> ClaireonTool_SlateScreenshot::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Capture target: \"window\" (top-level Slate window, default) or \"level_viewport\" (active level-editor 3D viewport)."));
		Prop->SetStringField(TEXT("default"), TEXT("window"));
		Properties->SetObjectField(TEXT("target"), Prop);
	}

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("For target=window: case-insensitive substring of the window title to capture. \"active\" (or omitted) captures the active top-level window."));
		Properties->SetObjectField(TEXT("window"), Prop);
	}

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Output filename without extension. Default: slate_<timestamp>. \".png\" is appended automatically."));
		Properties->SetObjectField(TEXT("filename"), Prop);
	}

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Output directory. Default: <Project>/Saved/Screenshots/."));
		Properties->SetObjectField(TEXT("directory"), Prop);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_SlateScreenshot::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!IsInGameThread())
	{
		return MakeErrorResult(TEXT("Slate screenshot must execute on the game thread"));
	}
	if (!FSlateApplication::IsInitialized())
	{
		return MakeErrorResult(TEXT("Slate application is not initialized"));
	}

	// All parameters are optional, so a missing arguments object means defaults.
	const TSharedPtr<FJsonObject> Args = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();

	// --- Resolve output path ------------------------------------------------
	FString Filename;
	if (!Args->TryGetStringField(TEXT("filename"), Filename) || Filename.IsEmpty())
	{
		const FDateTime Now = FDateTime::Now();
		Filename = FString::Printf(TEXT("slate_%04d%02d%02d_%02d%02d%02d"),
			Now.GetYear(), Now.GetMonth(), Now.GetDay(),
			Now.GetHour(), Now.GetMinute(), Now.GetSecond());
	}

	FString Directory;
	if (!Args->TryGetStringField(TEXT("directory"), Directory) || Directory.IsEmpty())
	{
		Directory = FPaths::ProjectSavedDir() / TEXT("Screenshots");
	}
	FPlatformFileManager::Get().GetPlatformFile().CreateDirectoryTree(*Directory);

	const FString FullPath = FPaths::ConvertRelativePathToFull(Directory / (Filename + TEXT(".png")));

	// --- Resolve target -----------------------------------------------------
	FString Target = TEXT("window");
	Args->TryGetStringField(TEXT("target"), Target);
	Target = Target.ToLower();

	TArray<FColor> ColorData;
	FIntVector Size(0, 0, 0);

	if (Target == TEXT("level_viewport"))
	{
		FLevelEditorModule* LevelEditorModule = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
		if (!LevelEditorModule)
		{
			return MakeErrorResult(TEXT("LevelEditor module is not loaded"));
		}

		TSharedPtr<ILevelEditor> LevelEditor = LevelEditorModule->GetFirstLevelEditor();
		if (!LevelEditor.IsValid())
		{
			return MakeErrorResult(TEXT("No level editor instance is available"));
		}

		TSharedPtr<SLevelViewport> LevelViewport = LevelEditor->GetActiveViewportInterface();
		if (!LevelViewport.IsValid())
		{
			return MakeErrorResult(TEXT("No active level viewport is available"));
		}

		FViewport* Viewport = LevelViewport->GetActiveViewport();
		if (!Viewport)
		{
			return MakeErrorResult(TEXT("Active level viewport has no FViewport"));
		}

		// Redraw so the read-back reflects the current scene state.
		Viewport->Draw();

		const FIntPoint ViewportSize = Viewport->GetSizeXY();
		if (ViewportSize.X <= 0 || ViewportSize.Y <= 0)
		{
			return MakeErrorResult(TEXT("Level viewport has zero size"));
		}

		const FIntRect CaptureRect(0, 0, ViewportSize.X, ViewportSize.Y);
		if (!Viewport->ReadPixels(ColorData, FReadSurfaceDataFlags(RCM_UNorm, CubeFace_MAX), CaptureRect))
		{
			return MakeErrorResult(TEXT("Failed to read pixels from the level viewport"));
		}
		Size = FIntVector(ViewportSize.X, ViewportSize.Y, 0);
	}
	else if (Target == TEXT("window"))
	{
		// Resolve which top-level Slate window to capture.
		FString WindowSel;
		Args->TryGetStringField(TEXT("window"), WindowSel);

		FSlateApplication& Slate = FSlateApplication::Get();
		TSharedPtr<SWindow> TargetWindow;

		if (WindowSel.IsEmpty() || WindowSel.Equals(TEXT("active"), ESearchCase::IgnoreCase))
		{
			TargetWindow = Slate.GetActiveTopLevelWindow();
			if (!ClaireonSlateScreenshotPrivate::IsCapturable(TargetWindow))
			{
				TargetWindow.Reset();
				const TArray<TSharedRef<SWindow>> TopWindows = Slate.GetTopLevelWindows();
				for (int32 Index = TopWindows.Num() - 1; Index >= 0; --Index)
				{
					if (ClaireonSlateScreenshotPrivate::IsCapturable(TopWindows[Index]))
					{
						TargetWindow = TopWindows[Index];
						break;
					}
				}
			}
		}
		else
		{
			const TArray<TSharedRef<SWindow>> TopWindows = Slate.GetTopLevelWindows();
			for (const TSharedRef<SWindow>& Window : TopWindows)
			{
				const FString Title = Window->GetTitle().ToString();
				if (Title.Contains(WindowSel, ESearchCase::IgnoreCase))
				{
					TargetWindow = Window;
					break;
				}
			}
		}

		if (!TargetWindow.IsValid())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("No matching Slate window found (selector: '%s'). Use \"active\" or a title substring."),
				WindowSel.IsEmpty() ? TEXT("active") : *WindowSel));
		}

		if (!ClaireonSlateScreenshotPrivate::IsCapturable(TargetWindow))
		{
			return MakeErrorResult(FString::Printf(TEXT("Window '%s' is not capturable: it must be visible, "
				"not minimized, have a valid native handle and a nonzero supported size."), *TargetWindow->GetTitle().ToString()));
		}

		FSlateRenderer* Renderer = Slate.GetRenderer();
		if (!Renderer) { return MakeErrorResult(TEXT("Slate renderer is unavailable")); }
		const bool bCaptured = ClaireonSlateScreenshotPrivate::CaptureAndRelease(
			[&]() { return Slate.TakeScreenshot(TargetWindow.ToSharedRef(), ColorData, Size); },
			[&](TArray<FColor>& DetachedBuffer)
			{ Renderer->PrepareToTakeScreenshot(FIntRect(0, 0, 0, 0), &DetachedBuffer, nullptr); },
			[&]() { Renderer->FlushCommands(); });
		if (!bCaptured)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("TakeScreenshot failed for window '%s'"), *TargetWindow->GetTitle().ToString()));
		}
	}
	else
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Unknown target '%s'. Use \"window\" or \"level_viewport\"."), *Target));
	}

	if (ColorData.Num() == 0 || Size.X <= 0 || Size.Y <= 0)
	{
		return MakeErrorResult(TEXT("Captured image is empty"));
	}

	if (ColorData.Num() < Size.X * Size.Y)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Captured pixel buffer (%d) smaller than reported size %dx%d"),
			ColorData.Num(), Size.X, Size.Y));
	}

	// Force alpha opaque; both read-back paths leave source alpha values.
	for (FColor& Pixel : ColorData)
	{
		Pixel.A = 255;
	}

	// --- Write the PNG ------------------------------------------------------
	// Both capture paths produce BGRA8 FColor, matching FImageView's BGRA8 constructor.
	const FImageView ImageView(ColorData.GetData(), Size.X, Size.Y);
	if (!FImageUtils::SaveImageByExtension(*FullPath, ImageView))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to write PNG to %s"), *FullPath));
	}

	int64 SizeBytes = 0;
	if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*FullPath))
	{
		SizeBytes = FPlatformFileManager::Get().GetPlatformFile().FileSize(*FullPath);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("file_path"), FullPath);
	Data->SetStringField(TEXT("target"), Target);
	Data->SetStringField(TEXT("resolution"), FString::Printf(TEXT("%dx%d"), Size.X, Size.Y));
	Data->SetNumberField(TEXT("size_bytes"), static_cast<double>(SizeBytes));

	const FString Summary = FString::Printf(TEXT("Screenshot (%s, %dx%d) saved to %s"),
		*Target, Size.X, Size.Y, *FullPath);
	return MakeSuccessResult(Data, Summary);
}

#if WITH_UNTESTED
#include "Untest.h"

UNTEST_UNIT_OPTS(Claireon, SlateScreenshot, CaptureAlwaysDetachesAndDrainsBeforeReturning,
	UNTEST_TIMEOUTMS(30000.0))
{
	// Model both a missed viewport (pending request) and an already queued readback.
	for (bool bCaptureSucceeded : {false, true})
	{
		TArray<FColor> Pixels;
		TArray<FColor>* Pending = nullptr;
		TArray<FColor>* QueuedReadback = nullptr;
		bool bViewportPending = false;
		FString Order;
		const bool Result = ClaireonSlateScreenshotPrivate::CaptureAndRelease(
			[&]()
			{
				Order += TEXT("capture;");
				Pending = QueuedReadback = &Pixels;
				bViewportPending = true;
				return bCaptureSucceeded;
			},
			[&](TArray<FColor>& Sink)
			{
				Order += TEXT("detach;");
				Pending = &Sink;
				bViewportPending = false;
			},
			[&]()
			{
				Order += TEXT("flush;");
				if (QueuedReadback)
				{
					QueuedReadback->Add(FColor::Red);
					QueuedReadback = nullptr;
				}
			});
		UNTEST_EXPECT_TRUE(Result == bCaptureSucceeded);
		UNTEST_EXPECT_EQ(Order, FString(TEXT("capture;flush;detach;flush;")));
		UNTEST_EXPECT_TRUE(Pending != nullptr && Pending != &Pixels);
		UNTEST_EXPECT_FALSE(bViewportPending);
		UNTEST_EXPECT_TRUE(QueuedReadback == nullptr);
		UNTEST_EXPECT_EQ(Pixels.Num(), 1);
		// The replacement storage remains alive after the wrapper returned.
		Pending->Add(FColor::Blue);
		Pending->Reset();
		UNTEST_EXPECT_EQ(Pixels.Num(), 1);
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SlateScreenshot, UnrenderableWindowsAreNotCaptureCandidates,
	UNTEST_TIMEOUTMS(30000.0))
{
	using namespace ClaireonSlateScreenshotPrivate;
	const FVector2D Size(800, 600);
	UNTEST_EXPECT_TRUE(IsCapturableState(true, true, true, true, false, Size));
	UNTEST_EXPECT_FALSE(IsCapturableState(false, false, true, true, false, Size));
	UNTEST_EXPECT_FALSE(IsCapturableState(true, false, true, true, false, Size));
	UNTEST_EXPECT_FALSE(IsCapturableState(true, true, false, true, false, Size));
	UNTEST_EXPECT_FALSE(IsCapturableState(true, true, true, false, false, Size));
	UNTEST_EXPECT_FALSE(IsCapturableState(true, true, true, true, true, Size));
	UNTEST_EXPECT_FALSE(IsCapturableState(true, true, true, true, false, FVector2D(0, 600)));
	UNTEST_EXPECT_FALSE(IsCapturableState(true, true, true, true, false, FVector2D(800, 0)));
	UNTEST_EXPECT_FALSE(IsCapturableState(true, true, true, true, false, FVector2D(MAX_int32, MAX_int32)));
	co_return;
}
#endif
