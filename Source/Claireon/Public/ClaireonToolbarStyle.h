// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FSlateStyleSet;

/**
 * Singleton style set for toolbar status dot rendering.
 * Registers a small circle brush and color resources used by the
 * toolbar button overlay in FClaireonModule::RegisterMenus().
 *
 * Lifecycle: Initialize() in FClaireonModule::StartupModule(),
 *            Shutdown()   in FClaireonModule::ShutdownModule().
 */
class FClaireonToolbarStyle
{
public:
	static void Initialize();
	static void Shutdown();
	static const FSlateStyleSet& Get();
	static FName GetStyleSetName();

	static FLinearColor GetRunningColor();
	static FLinearColor GetStoppedColor();

	/** Blink colors used while a request is in-flight (or within the min-display window). */
	static FLinearColor GetProcessingColorA();
	static FLinearColor GetProcessingColorB();

	/** Minimum seconds the processing blink stays visible after the last request arrives. */
	static float GetProcessingMinDurationSeconds();

	/** Seconds per half-cycle of the A/B blink (full cycle = 2x this). */
	static float GetProcessingBlinkHalfPeriodSeconds();

private:
	static TSharedRef<FSlateStyleSet> Create();
	static TSharedPtr<FSlateStyleSet> StyleInstance;
};
