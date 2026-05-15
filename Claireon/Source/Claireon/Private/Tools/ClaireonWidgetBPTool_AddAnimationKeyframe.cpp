// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Dispatch Backlog: https://example.com/internal-doc
// Animation op stub. Expected UMG APIs: UWidgetAnimation / UWidgetBlueprint::Animations / UWidgetAnimationBinding.
// Follow-up PR implements Execute; schema / description / keywords are already wired.

#include "Tools/ClaireonWidgetBPTool_AddAnimationKeyframe.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_AddAnimationKeyframe::GetName() const
{
    return TEXT("claireon.widgetbp_add_animation_keyframe");
}

FString ClaireonWidgetBPTool_AddAnimationKeyframe::GetDescription() const
{
    return TEXT("[Backlogged] Add a keyframe to a widget animation track. Implementation deferred to Dispatch Backlog: https://example.com/internal-doc.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_AddAnimationKeyframe::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the UWidgetAnimation."), true);
    Builder.AddString(TEXT("track_name"), TEXT("Track identifier (widget + property)."), true);
    Builder.AddNumber(TEXT("time"), TEXT("Keyframe time (seconds)."), true);
    Builder.AddString(TEXT("value"), TEXT("Keyframe value (string form, decoded per track type)."), true);
    Builder.AddString(TEXT("interpolation"), TEXT("Optional interpolation: 'Linear', 'Cubic', 'Constant'."));
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_AddAnimationKeyframe::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("keyframe"), TEXT("key"), TEXT("add"), TEXT("sequence") };
}

FToolResult ClaireonWidgetBPTool_AddAnimationKeyframe::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    return MakeErrorResult(TEXT("add_animation_keyframe is not yet implemented; tracked in Dispatch Backlog: https://example.com/internal-doc"));
}
