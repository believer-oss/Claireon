// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_CreateAnimation.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonWidgetAnimationHandlers.h"
#include "Animation/WidgetAnimation.h"
#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_CreateAnimation::GetOperation() const { return TEXT("create_animation"); }

FString ClaireonWidgetBPTool_CreateAnimation::GetDescription() const
{
    return TEXT("Create a new UWidgetAnimation on the Widget Blueprint (name, duration). Session-mode tool: open via widgetbp_open first.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_CreateAnimation::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the new UWidgetAnimation."), true);
    Builder.AddNumber(TEXT("duration"), TEXT("Total animation duration in seconds."));
    Builder.AddString(TEXT("display_label"), TEXT("Optional display label; defaults to animation_name."));
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_CreateAnimation::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("create"), TEXT("sequence"), TEXT("track"), TEXT("timeline") };
}

FToolResult ClaireonWidgetBPTool_CreateAnimation::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult BeginError;
    if (!BeginSessionOp(Arguments, TEXT("create_animation"), Params, SessionId, Data, BeginError))
    {
        return BeginError;
    }
    UWidgetBlueprint* WBP = Data ? Data->WidgetBlueprint.Get() : nullptr;
    if (!WBP)
    {
        return MakeErrorResult(TEXT("widget blueprint unavailable on session"));
    }

    FString AnimationName;
    if (!Params->TryGetStringField(TEXT("animation_name"), AnimationName) || AnimationName.IsEmpty())
    {
        return MakeErrorResult(TEXT("animation_name is required"));
    }
    double Duration = 5.0;
    Params->TryGetNumberField(TEXT("duration"), Duration);
    FString DisplayLabel;
    Params->TryGetStringField(TEXT("display_label"), DisplayLabel);

    FScopedTransaction Transaction(NSLOCTEXT("Claireon", "CreateWidgetAnimation", "Create Widget Animation"));
    UWidgetAnimation* NewAnim = nullptr;
    FString ApplyError;
    if (!Claireon::WidgetAnimation::ApplyCreateAnimation(WBP, AnimationName, static_cast<float>(Duration), DisplayLabel, NewAnim, ApplyError))
    {
        Transaction.Cancel();
        return MakeErrorResult(ApplyError);
    }
    Data->bModified = true;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), NewAnim->GetFName().ToString());
    ResultObj->SetNumberField(TEXT("start_time"), NewAnim->GetStartTime());
    ResultObj->SetNumberField(TEXT("end_time"), NewAnim->GetEndTime());
    ResultObj->SetStringField(TEXT("display_label"), NewAnim->GetDisplayLabel());

    return MakeSuccessResult(ResultObj,
        FString::Printf(TEXT("Created animation '%s' (duration=%.2fs)"), *AnimationName, static_cast<float>(Duration)));
}

