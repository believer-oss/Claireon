// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_GetAnimationDetails.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonWidgetAnimationHandlers.h"
#include "ClaireonWidgetHelpers.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Dom/JsonObject.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "WidgetBlueprint.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_GetAnimationDetails::GetOperation() const { return TEXT("get_animation_details"); }

FString ClaireonWidgetBPTool_GetAnimationDetails::GetDescription() const
{
    return TEXT("Inspect tracks, bindings, and keyframes on a UWidgetAnimation.");
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
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult BeginError;
    if (!BeginSessionOp(Arguments, TEXT("get_animation_details"), Params, SessionId, Data, BeginError))
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
    UWidgetAnimation* Anim = FindWidgetAnimationByName(WBP, AnimationName);
    if (!Anim)
    {
        return MakeErrorResult(FString::Printf(TEXT("animation '%s' not found on %s"), *AnimationName, *WBP->GetName()));
    }

    // Delegate the heavy-lifting to the shared helper.
    TSharedPtr<FJsonObject> ResultObj = ClaireonWidgetHelpers::SerializeAnimationDetails(Anim);
    if (!ResultObj.IsValid())
    {
        ResultObj = MakeShared<FJsonObject>();
    }
    // Ensure the [D1] slot_widget_name readback is present with NAME_None -> null semantics.
    ResultObj->SetStringField(TEXT("name"), Anim->GetFName().ToString());
    ResultObj->SetStringField(TEXT("display_label"), Anim->GetDisplayLabel());
    ResultObj->SetNumberField(TEXT("start_time"), Anim->GetStartTime());
    ResultObj->SetNumberField(TEXT("end_time"), Anim->GetEndTime());

    TArray<TSharedPtr<FJsonValue>> BindingArr;
    for (const FWidgetAnimationBinding& Binding : Anim->GetBindings())
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("widget_name"), Binding.WidgetName.ToString());
        if (Binding.SlotWidgetName.IsNone())
        {
            Obj->SetField(TEXT("slot_widget_name"), MakeShared<FJsonValueNull>());
        }
        else
        {
            Obj->SetStringField(TEXT("slot_widget_name"), Binding.SlotWidgetName.ToString());
        }
        Obj->SetStringField(TEXT("animation_guid"), Binding.AnimationGuid.ToString(EGuidFormats::DigitsWithHyphens));
        BindingArr.Add(MakeShared<FJsonValueObject>(Obj));
    }
    ResultObj->SetArrayField(TEXT("bindings"), BindingArr);

    TArray<TSharedPtr<FJsonValue>> TrackArr;
    if (UMovieScene* MS = Anim->GetMovieScene())
    {
        for (const FMovieSceneBinding& MSBinding : MS->GetBindings())
        {
            for (const UMovieSceneTrack* Track : MSBinding.GetTracks())
            {
                if (!Track)
                {
                    continue;
                }
                TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
                Obj->SetStringField(TEXT("track_class"), Track->GetClass()->GetName());
                Obj->SetStringField(TEXT("track_name"), Track->GetTrackName().ToString());
                Obj->SetNumberField(TEXT("section_count"), Track->GetAllSections().Num());
                TrackArr.Add(MakeShared<FJsonValueObject>(Obj));
            }
        }
    }
    ResultObj->SetArrayField(TEXT("tracks"), TrackArr);

    return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Details for animation '%s'"), *AnimationName));
}

