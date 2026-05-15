// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Dispatch Backlog: https://example.com/internal-doc
// Animation op stub. Expected UMG APIs: UWidgetAnimation / UWidgetBlueprint::Animations / UWidgetAnimationBinding.
// Follow-up PR implements Execute; schema / description / keywords are already wired.

#include "Tools/ClaireonWidgetBPTool_RenameAnimation.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_RenameAnimation::GetName() const
{
    return TEXT("claireon.widgetbp_rename_animation");
}

FString ClaireonWidgetBPTool_RenameAnimation::GetDescription() const
{
    return TEXT("[Backlogged] Rename a UWidgetAnimation. Implementation deferred to Dispatch Backlog: https://example.com/internal-doc.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_RenameAnimation::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Current animation name."), true);
    Builder.AddString(TEXT("new_name"), TEXT("New animation name."), true);
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_RenameAnimation::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("rename"), TEXT("sequence") };
}

FToolResult ClaireonWidgetBPTool_RenameAnimation::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    return MakeErrorResult(TEXT("rename_animation is not yet implemented; tracked in Dispatch Backlog: https://example.com/internal-doc"));
}
