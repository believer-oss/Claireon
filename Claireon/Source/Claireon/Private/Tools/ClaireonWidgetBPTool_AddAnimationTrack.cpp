// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// the backlog: https://example.com/internal-doc
// Animation op stub. Expected UMG APIs: UWidgetAnimation / UWidgetBlueprint::Animations / UWidgetAnimationBinding.
// Follow-up PR implements Execute; schema / description / keywords are already wired.

#include "Tools/ClaireonWidgetBPTool_AddAnimationTrack.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_AddAnimationTrack::GetName() const
{
    return TEXT("claireon.widgetbp_add_animation_track");
}

FString ClaireonWidgetBPTool_AddAnimationTrack::GetDescription() const
{
    return TEXT("[Backlogged] Add a track to a UWidgetAnimation. Implementation deferred to the backlog: https://example.com/internal-doc.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_AddAnimationTrack::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the UWidgetAnimation."), true);
    Builder.AddString(TEXT("widget_name"), TEXT("Widget the new track targets."), true);
    Builder.AddString(TEXT("property_name"), TEXT("Widget property the track drives."), true);
    Builder.AddString(TEXT("track_type"), TEXT("Sequencer track type (e.g. 'Float', 'Color', 'Vector2D')."));
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_AddAnimationTrack::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("track"), TEXT("add"), TEXT("sequence") };
}

FToolResult ClaireonWidgetBPTool_AddAnimationTrack::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    return MakeErrorResult(TEXT("add_animation_track is not yet implemented; tracked in the backlog: https://example.com/internal-doc"));
}
