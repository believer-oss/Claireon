// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// the backlog: https://example.com/internal-doc
// Animation op stub. Expected UMG APIs: UWidgetAnimation / UWidgetBlueprint::Animations / UWidgetAnimationBinding.
// Follow-up PR implements Execute; schema / description / keywords are already wired.

#include "Tools/ClaireonWidgetBPTool_AddAnimationBinding.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_AddAnimationBinding::GetName() const
{
    return TEXT("claireon.widgetbp_add_animation_binding");
}

FString ClaireonWidgetBPTool_AddAnimationBinding::GetDescription() const
{
    return TEXT("[Backlogged] Bind a widget to a UWidgetAnimation (UWidgetAnimationBinding). Implementation deferred to the backlog: https://example.com/internal-doc.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_AddAnimationBinding::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the UWidgetAnimation."), true);
    Builder.AddString(TEXT("widget_name"), TEXT("Widget in the tree to bind."), true);
    Builder.AddString(TEXT("property_name"), TEXT("Optional property on the widget to bind (for property-style bindings)."));
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_AddAnimationBinding::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("binding"), TEXT("bind"), TEXT("sequence") };
}

FToolResult ClaireonWidgetBPTool_AddAnimationBinding::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    return MakeErrorResult(TEXT("add_animation_binding is not yet implemented; tracked in the backlog: https://example.com/internal-doc"));
}
