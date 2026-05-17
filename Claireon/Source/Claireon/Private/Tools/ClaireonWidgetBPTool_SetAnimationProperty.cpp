// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_SetAnimationProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonWidgetAnimationHandlers.h"
#include "Animation/WidgetAnimation.h"
#include "Dom/JsonObject.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/DefaultValueHelper.h"
#include "MovieScene.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_SetAnimationProperty::GetOperation() const { return TEXT("set_animation_property"); }

FString ClaireonWidgetBPTool_SetAnimationProperty::GetDescription() const
{
    return TEXT("Set a property on a UWidgetAnimation (display_label, duration, display_rate).");
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
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult BeginError;
    if (!BeginSessionOp(Arguments, TEXT("set_animation_property"), Params, SessionId, Data, BeginError))
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
    UWidgetAnimation* Anim = Claireon::WidgetAnimation::FindWidgetAnimationByName(WBP, AnimationName);
    if (!Anim)
    {
        return MakeErrorResult(FString::Printf(TEXT("animation '%s' not found on %s"), *AnimationName, *WBP->GetName()));
    }

    FString PropertyName;
    FString Value;
    Params->TryGetStringField(TEXT("property_name"), PropertyName);
    if (!Params->TryGetStringField(TEXT("value"), Value))
    {
        // Accept numeric `duration` convenience for legacy callers.
        double DurationNum = 0.0;
        if (Params->TryGetNumberField(TEXT("duration"), DurationNum))
        {
            PropertyName = TEXT("duration");
            Value = FString::SanitizeFloat(DurationNum);
        }
    }
    if (PropertyName.IsEmpty())
    {
        return MakeErrorResult(TEXT("property_name is required"));
    }

    UMovieScene* MS = Anim->GetMovieScene();
    if (!MS)
    {
        return MakeErrorResult(TEXT("animation has no MovieScene"));
    }

    const FString Normalized = PropertyName.ToLower();
    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("property_name"), PropertyName);
    ResultObj->SetStringField(TEXT("value"), Value);

    if (Normalized == TEXT("display_label"))
    {
        FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SetWidgetAnimDisplayLabel", "Set Widget Animation Display Label"));
        Anim->Modify();
        Anim->SetDisplayLabel(Value);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        Data->bModified = true;
        return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Set display_label on '%s' -> '%s'"), *AnimationName, *Value));
    }
    if (Normalized == TEXT("duration"))
    {
        double DurationD = 0.0;
        if (!FDefaultValueHelper::ParseDouble(Value, DurationD))
        {
            return MakeErrorResult(TEXT("duration value must parse as a number"));
        }
        const float Duration = FMath::Max(static_cast<float>(DurationD), 0.01f);
        FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SetWidgetAnimDuration", "Set Widget Animation Duration"));
        MS->Modify();
        const FFrameRate TickResolution = MS->GetTickResolution();
        const FFrameNumber EndFrame = (Duration * TickResolution).FloorToFrame();
        MS->SetPlaybackRange(TRange<FFrameNumber>(FFrameNumber(0), EndFrame + FFrameNumber(1)));
#if WITH_EDITORONLY_DATA
        FMovieSceneEditorData& EditorData = MS->GetEditorData();
        EditorData.WorkStart = 0.0;
        EditorData.WorkEnd = Duration;
#endif
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        Data->bModified = true;
        return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Set duration on '%s' -> %.2fs"), *AnimationName, Duration));
    }
    if (Normalized == TEXT("display_rate"))
    {
        int32 Num = 0;
        int32 Denom = 1;
        if (!Value.Split(TEXT("/"), nullptr, nullptr))
        {
            float FloatRate = 0.f;
            if (!FDefaultValueHelper::ParseFloat(Value, FloatRate) || FloatRate <= 0.f)
            {
                return MakeErrorResult(TEXT("display_rate value must be a positive number or 'num/denom'"));
            }
            Num = FMath::RoundToInt(FloatRate);
            Denom = 1;
        }
        else
        {
            FString NumStr;
            FString DenomStr;
            Value.Split(TEXT("/"), &NumStr, &DenomStr);
            if (!FDefaultValueHelper::ParseInt(NumStr, Num) || !FDefaultValueHelper::ParseInt(DenomStr, Denom)
                || Num <= 0 || Denom <= 0)
            {
                return MakeErrorResult(TEXT("display_rate 'num/denom' must be positive integers"));
            }
        }
        FScopedTransaction Transaction(NSLOCTEXT("Claireon", "SetWidgetAnimDisplayRate", "Set Widget Animation Display Rate"));
        MS->Modify();
        MS->SetDisplayRate(FFrameRate(Num, Denom));
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
        Data->bModified = true;
        return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Set display_rate on '%s' -> %d/%d"), *AnimationName, Num, Denom));
    }
    if (Normalized == TEXT("loop_mode") || Normalized == TEXT("loop_count"))
    {
        return MakeErrorResult(TEXT("loop_mode / loop_count are not persisted on UWidgetAnimation; set at play time via UWidgetAnimation::PlayAnimation(..., LoopMode, ...) or in the blueprint graph"));
    }
    return MakeErrorResult(FString::Printf(TEXT("unknown animation property: %s"), *PropertyName));
}

