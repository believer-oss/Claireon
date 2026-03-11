// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FSlateStyleSet;

/**
 * Singleton style set for REPL rich text rendering.
 * Registers named FTextBlockStyle entries (headers, bold, italic, code, asset links)
 * used by SRichTextBlock in the REPL widget.
 *
 * Lifecycle: Initialize() in FClaireonModule::StartupModule(),
 *            Shutdown()   in FClaireonModule::ShutdownModule().
 */
class FClaireonRichTextStyle
{
public:
	static void Initialize();
	static void Shutdown();
	static const FSlateStyleSet& Get();
	static FName GetStyleSetName();

private:
	static TSharedRef<FSlateStyleSet> Create();
	static TSharedPtr<FSlateStyleSet> StyleInstance;
};
