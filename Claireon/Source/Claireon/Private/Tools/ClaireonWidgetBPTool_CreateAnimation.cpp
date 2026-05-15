// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// the backlog: https://example.com/internal-doc
// Animation op stub. Expected UMG APIs: UWidgetAnimation / UWidgetBlueprint::Animations / UWidgetAnimationBinding.
// Follow-up PR implements Execute; schema / description / keywords are already wired.

#include "Tools/ClaireonWidgetBPTool_CreateAnimation.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_CreateAnimation::GetName() const
{
    return TEXT("claireon.widgetbp_create_animation");
}

FString ClaireonWidgetBPTool_CreateAnimation::GetDescription() const
{
    return TEXT("[Backlogged] Create a new UWidgetAnimation on the Widget Blueprint (name, duration). Implementation deferred to the backlog: https://example.com/internal-doc.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_CreateAnimation::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the new UWidgetAnimation."), true);
    Builder.AddNumber(TEXT("duration"), TEXT("Total animation duration in seconds."));
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_CreateAnimation::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("create"), TEXT("sequence"), TEXT("track"), TEXT("timeline") };
}

FToolResult ClaireonWidgetBPTool_CreateAnimation::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    return MakeErrorResult(TEXT("create_animation is not yet implemented; tracked in the backlog: https://example.com/internal-doc"));
}
