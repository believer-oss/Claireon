// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Dispatch Backlog: https://example.com/internal-doc
// Animation op stub. Expected UMG APIs: UWidgetAnimation / UWidgetBlueprint::Animations / UWidgetAnimationBinding.
// Follow-up PR implements Execute; schema / description / keywords are already wired.

#include "Tools/ClaireonWidgetBPTool_SetAnimationProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_SetAnimationProperty::GetName() const
{
    return TEXT("claireon.widgetbp_set_animation_property");
}

FString ClaireonWidgetBPTool_SetAnimationProperty::GetDescription() const
{
    return TEXT("[Backlogged] Set a property on a UWidgetAnimation (duration, loop count, etc.). Implementation deferred to Dispatch Backlog: https://example.com/internal-doc.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_SetAnimationProperty::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the UWidgetAnimation."), true);
    Builder.AddString(TEXT("property_name"), TEXT("Property on the animation (e.g. duration)."), true);
    Builder.AddString(TEXT("value"), TEXT("String-form value (imported via the property's text format)."), true);
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_SetAnimationProperty::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("property"), TEXT("set"), TEXT("duration"), TEXT("loop") };
}

FToolResult ClaireonWidgetBPTool_SetAnimationProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    return MakeErrorResult(TEXT("set_animation_property is not yet implemented; tracked in Dispatch Backlog: https://example.com/internal-doc"));
}
