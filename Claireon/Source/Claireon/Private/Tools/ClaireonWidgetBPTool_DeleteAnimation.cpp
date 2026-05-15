// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Dispatch Backlog: https://example.com/internal-doc
// Animation op stub. Expected UMG APIs: UWidgetAnimation / UWidgetBlueprint::Animations / UWidgetAnimationBinding.
// Follow-up PR implements Execute; schema / description / keywords are already wired.

#include "Tools/ClaireonWidgetBPTool_DeleteAnimation.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_DeleteAnimation::GetName() const
{
    return TEXT("claireon.widgetbp_delete_animation");
}

FString ClaireonWidgetBPTool_DeleteAnimation::GetDescription() const
{
    return TEXT("[Backlogged] Delete a UWidgetAnimation by name. Implementation deferred to Dispatch Backlog: https://example.com/internal-doc.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_DeleteAnimation::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the UWidgetAnimation to delete."), true);
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_DeleteAnimation::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("delete"), TEXT("remove"), TEXT("sequence") };
}

FToolResult ClaireonWidgetBPTool_DeleteAnimation::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    return MakeErrorResult(TEXT("delete_animation is not yet implemented; tracked in Dispatch Backlog: https://example.com/internal-doc"));
}
