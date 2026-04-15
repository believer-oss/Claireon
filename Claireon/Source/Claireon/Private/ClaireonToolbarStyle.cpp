// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonToolbarStyle.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Brushes/SlateRoundedBoxBrush.h"

TSharedPtr<FSlateStyleSet> FClaireonToolbarStyle::StyleInstance = nullptr;

// Toolbar status dot colors (file-scope, prefixed for unity builds)
const FLinearColor TBS_Green(0.2f, 0.8f, 0.2f); // Server running
const FLinearColor TBS_Red(0.9f, 0.2f, 0.2f);	// Server stopped/error
const FLinearColor TBS_Cyan(0.2f, 0.9f, 1.0f);	// Processing - phase A (bright cyan)
const FLinearColor TBS_Blue(0.1f, 0.4f, 1.0f);	// Processing - phase B (deeper blue)

void FClaireonToolbarStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FClaireonToolbarStyle::Shutdown()
{
	if (StyleInstance.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
		ensure(StyleInstance.IsUnique());
		StyleInstance.Reset();
	}
}

FName FClaireonToolbarStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("ClaireonToolbar"));
	return StyleSetName;
}

TSharedRef<FSlateStyleSet> FClaireonToolbarStyle::Create()
{
	TSharedRef<FSlateStyleSet> StyleRef =
		MakeShareable(new FSlateStyleSet(FClaireonToolbarStyle::GetStyleSetName()));

	FSlateStyleSet& Style = StyleRef.Get();

	// Status dot brush -- small filled circle, 8x8
	Style.Set(TEXT("ClaireonToolbar.StatusDot"),
		new FSlateRoundedBoxBrush(FLinearColor::White, 4.0f, FVector2D(8.0f, 8.0f)));

	return StyleRef;
}

const FSlateStyleSet& FClaireonToolbarStyle::Get()
{
	check(StyleInstance.IsValid());
	return *StyleInstance;
}

FLinearColor FClaireonToolbarStyle::GetRunningColor()
{
	return TBS_Green;
}

FLinearColor FClaireonToolbarStyle::GetStoppedColor()
{
	return TBS_Red;
}

FLinearColor FClaireonToolbarStyle::GetProcessingColorA()
{
	return TBS_Cyan;
}

FLinearColor FClaireonToolbarStyle::GetProcessingColorB()
{
	return TBS_Blue;
}

float FClaireonToolbarStyle::GetProcessingMinDurationSeconds()
{
	// Long enough for a full A-B-A cycle to be visible even on instant requests.
	return 0.6f;
}

float FClaireonToolbarStyle::GetProcessingBlinkHalfPeriodSeconds()
{
	// 150ms per phase => 300ms full cycle (~3.3 Hz alternation).
	return 0.15f;
}
