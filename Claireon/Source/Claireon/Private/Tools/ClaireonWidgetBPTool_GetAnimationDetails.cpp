// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// the backlog: https://example.com/internal-doc
// Animation op stub. Expected UMG APIs: UWidgetAnimation / UWidgetBlueprint::Animations / UWidgetAnimationBinding.
// Follow-up PR implements Execute; schema / description / keywords are already wired.

#include "Tools/ClaireonWidgetBPTool_GetAnimationDetails.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_GetAnimationDetails::GetName() const
{
    return TEXT("claireon.widgetbp_get_animation_details");
}

FString ClaireonWidgetBPTool_GetAnimationDetails::GetDescription() const
{
    return TEXT("[Backlogged] Inspect tracks, bindings, and keyframes on a UWidgetAnimation. Implementation deferred to the backlog: https://example.com/internal-doc.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_GetAnimationDetails::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the UWidgetAnimation to inspect."), true);
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_GetAnimationDetails::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("details"), TEXT("inspect"), TEXT("tracks"), TEXT("keyframes") };
}

FToolResult ClaireonWidgetBPTool_GetAnimationDetails::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    return MakeErrorResult(TEXT("get_animation_details is not yet implemented; tracked in the backlog: https://example.com/internal-doc"));
}
