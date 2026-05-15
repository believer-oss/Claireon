// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// the backlog: https://example.com/internal-doc
// Animation op stub. Expected UMG APIs: UWidgetAnimation / UWidgetBlueprint::Animations / UWidgetAnimationBinding.
// Follow-up PR implements Execute; schema / description / keywords are already wired.

#include "Tools/ClaireonWidgetBPTool_RemoveAnimationKeyframe.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_RemoveAnimationKeyframe::GetName() const
{
    return TEXT("claireon.widgetbp_remove_animation_keyframe");
}

FString ClaireonWidgetBPTool_RemoveAnimationKeyframe::GetDescription() const
{
    return TEXT("[Backlogged] Remove a keyframe from a widget animation track. Implementation deferred to the backlog: https://example.com/internal-doc.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_RemoveAnimationKeyframe::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the UWidgetAnimation."), true);
    Builder.AddString(TEXT("track_name"), TEXT("Track identifier (widget + property)."), true);
    Builder.AddNumber(TEXT("time"), TEXT("Keyframe time (seconds) to remove."), true);
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_RemoveAnimationKeyframe::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("keyframe"), TEXT("remove"), TEXT("sequence") };
}

FToolResult ClaireonWidgetBPTool_RemoveAnimationKeyframe::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    return MakeErrorResult(TEXT("remove_animation_keyframe is not yet implemented; tracked in the backlog: https://example.com/internal-doc"));
}
