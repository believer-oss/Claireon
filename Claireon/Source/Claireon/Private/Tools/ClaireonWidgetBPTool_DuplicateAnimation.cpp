// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Dispatch Backlog: https://example.com/internal-doc
// Animation op stub. Expected UMG APIs: UWidgetAnimation / UWidgetBlueprint::Animations / UWidgetAnimationBinding.
// Follow-up PR implements Execute; schema / description / keywords are already wired.

#include "Tools/ClaireonWidgetBPTool_DuplicateAnimation.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_DuplicateAnimation::GetName() const
{
    return TEXT("claireon.widgetbp_duplicate_animation");
}

FString ClaireonWidgetBPTool_DuplicateAnimation::GetDescription() const
{
    return TEXT("[Backlogged] Duplicate a UWidgetAnimation under a new name. Implementation deferred to Dispatch Backlog: https://example.com/internal-doc.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_DuplicateAnimation::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the source animation to duplicate."), true);
    Builder.AddString(TEXT("new_name"), TEXT("Name of the duplicate."), true);
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_DuplicateAnimation::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("duplicate"), TEXT("copy"), TEXT("sequence") };
}

FToolResult ClaireonWidgetBPTool_DuplicateAnimation::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    return MakeErrorResult(TEXT("duplicate_animation is not yet implemented; tracked in Dispatch Backlog: https://example.com/internal-doc"));
}
