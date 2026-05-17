// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_AddAnimationKeyframe.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSequenceEditHandlers.h"
#include "ClaireonWidgetAnimationHandlers.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Dom/JsonObject.h"
#include "KeyParams.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MovieScene.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_AddAnimationKeyframe::GetOperation() const { return TEXT("add_animation_keyframe"); }

FString ClaireonWidgetBPTool_AddAnimationKeyframe::GetDescription() const
{
    return TEXT("Add a keyframe to a widget animation track.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_AddAnimationKeyframe::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the UWidgetAnimation."), true);
    Builder.AddString(TEXT("widget_name"), TEXT("Widget whose binding holds the target track."));
    Builder.AddString(TEXT("property_name"), TEXT("Property name (or track name) on the binding. property_path accepted as an alias."));
    Builder.AddNumber(TEXT("time"), TEXT("Keyframe time (seconds)."), true);
    Builder.AddString(TEXT("value"), TEXT("Keyframe value (JSON payload; decoded per channel type)."), true);
    Builder.AddString(TEXT("interpolation"), TEXT("Optional interpolation: 'Linear', 'Cubic', 'Constant' (float/double/integer channels only)."));
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_AddAnimationKeyframe::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("keyframe"), TEXT("key"), TEXT("add"), TEXT("sequence") };
}

namespace ClaireonWidgetKeyframe
{
    static bool ParseInterpolation(const FString& In, EMovieSceneKeyInterpolation& Out)
    {
        const FString Norm = In.ToLower();
        if (Norm == TEXT("linear")) { Out = EMovieSceneKeyInterpolation::Linear; return true; }
        if (Norm == TEXT("constant")) { Out = EMovieSceneKeyInterpolation::Constant; return true; }
        if (Norm == TEXT("cubic") || Norm == TEXT("auto")) { Out = EMovieSceneKeyInterpolation::Auto; return true; }
        if (Norm == TEXT("break")) { Out = EMovieSceneKeyInterpolation::Break; return true; }
        if (Norm == TEXT("user")) { Out = EMovieSceneKeyInterpolation::User; return true; }
        if (Norm == TEXT("smartauto")) { Out = EMovieSceneKeyInterpolation::SmartAuto; return true; }
        return false;
    }

    static UMovieSceneSection* ResolveFirstSection(UWidgetAnimation* Anim, const FGuid& BindingGuid, const FString& TrackNameOrProperty, UMovieScene*& OutMS, UMovieSceneTrack*& OutTrack)
    {
        OutMS = Anim ? Anim->GetMovieScene() : nullptr;
        OutTrack = nullptr;
        if (!OutMS)
        {
            return nullptr;
        }
        const FMovieSceneBinding* MSBinding = OutMS->GetBindings().FindByPredicate(
            [&](const FMovieSceneBinding& B) { return B.GetObjectGuid() == BindingGuid; });
        if (!MSBinding)
        {
            return nullptr;
        }
        const FName Wanted(*TrackNameOrProperty);
        for (UMovieSceneTrack* Track : MSBinding->GetTracks())
        {
            if (!Track) { continue; }
            if (TrackNameOrProperty.IsEmpty() || Track->GetTrackName() == Wanted
                || Track->GetClass()->GetName() == TrackNameOrProperty)
            {
                OutTrack = Track;
                break;
            }
        }
        if (!OutTrack && MSBinding->GetTracks().Num() > 0)
        {
            // Fallback: first track on binding.
            OutTrack = MSBinding->GetTracks()[0];
        }
        if (!OutTrack)
        {
            return nullptr;
        }
        const TArray<UMovieSceneSection*>& Sections = OutTrack->GetAllSections();
        return Sections.Num() > 0 ? Sections[0] : nullptr;
    }
}

FToolResult ClaireonWidgetBPTool_AddAnimationKeyframe::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult BeginError;
    if (!BeginSessionOp(Arguments, TEXT("add_animation_keyframe"), Params, SessionId, Data, BeginError))
    {
        return BeginError;
    }
    using namespace ClaireonWidgetKeyframe;

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
    double TimeSeconds = 0.0;
    Params->TryGetNumberField(TEXT("time"), TimeSeconds);

    // Accept either explicit track_name or the widget+property combo used by the
    // spec applicator and existing test coverage.
    FString TrackName;
    Params->TryGetStringField(TEXT("track_name"), TrackName);
    FString WidgetName;
    Params->TryGetStringField(TEXT("widget_name"), WidgetName);
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        Params->TryGetStringField(TEXT("property_path"), PropertyName);
    }

    UWidgetAnimation* Anim = Claireon::WidgetAnimation::FindWidgetAnimationByName(WBP, AnimationName);
    if (!Anim)
    {
        return MakeErrorResult(FString::Printf(TEXT("animation '%s' not found on %s"), *AnimationName, *WBP->GetName()));
    }

    FGuid BindingGuid;
    if (!WidgetName.IsEmpty())
    {
        const FName WFName(*WidgetName);
        for (const FWidgetAnimationBinding& B : Anim->GetBindings())
        {
            if (B.WidgetName == WFName) { BindingGuid = B.AnimationGuid; break; }
        }
    }
    else if (Anim->GetBindings().Num() > 0)
    {
        BindingGuid = Anim->GetBindings()[0].AnimationGuid;
    }
    if (!BindingGuid.IsValid())
    {
        return MakeErrorResult(TEXT("no binding resolved; provide widget_name with an existing binding"));
    }

    UMovieScene* MS = nullptr;
    UMovieSceneTrack* Track = nullptr;
    UMovieSceneSection* Section = ResolveFirstSection(Anim, BindingGuid,
        TrackName.IsEmpty() ? PropertyName : TrackName, MS, Track);
    if (!Section)
    {
        return MakeErrorResult(TEXT("no section found on target track; call add_animation_track first"));
    }

    const FFrameRate TickResolution = MS->GetTickResolution();
    const FFrameNumber FrameNumber = (TimeSeconds * TickResolution).FloorToFrame();

    // Accept numeric, bool, or string value payloads; serialize to a JSON-compatible
    // scalar string that ApplyAddKeyframe knows how to coerce.
    FString ValueJson;
    if (Params->HasTypedField<EJson::Number>(TEXT("value")))
    {
        double NumVal = 0.0;
        Params->TryGetNumberField(TEXT("value"), NumVal);
        ValueJson = FString::SanitizeFloat(NumVal);
    }
    else if (Params->HasTypedField<EJson::Boolean>(TEXT("value")))
    {
        bool BoolVal = false;
        Params->TryGetBoolField(TEXT("value"), BoolVal);
        ValueJson = BoolVal ? TEXT("true") : TEXT("false");
    }
    else
    {
        Params->TryGetStringField(TEXT("value"), ValueJson);
    }

    FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AddWidgetAnimationKeyframe", "Add Widget Animation Keyframe"));
    FString ApplyError;
    if (!Claireon::SequenceEdit::ApplyAddKeyframe(Section, FrameNumber, ValueJson, ApplyError))
    {
        Transaction.Cancel();
        return MakeErrorResult(ApplyError);
    }

    FString InterpolationStr;
    if (Params->TryGetStringField(TEXT("interpolation"), InterpolationStr) && !InterpolationStr.IsEmpty())
    {
        EMovieSceneKeyInterpolation InterpMode = EMovieSceneKeyInterpolation::Auto;
        if (!ParseInterpolation(InterpolationStr, InterpMode))
        {
            Transaction.Cancel();
            return MakeErrorResult(FString::Printf(TEXT("unknown interpolation '%s'; supported: Constant, Linear, Cubic"), *InterpolationStr));
        }
        FString InterpError;
        if (!Claireon::SequenceEdit::ApplySetKeyInterpMode(Section, FrameNumber, InterpMode, InterpError))
        {
            Transaction.Cancel();
            return MakeErrorResult(InterpError);
        }
    }

    WBP->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    Data->bModified = true;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetNumberField(TEXT("time"), TimeSeconds);
    ResultObj->SetNumberField(TEXT("frame"), FrameNumber.Value);
    ResultObj->SetStringField(TEXT("value_echoed"), ValueJson);
    if (InterpolationStr.IsEmpty())
    {
        ResultObj->SetField(TEXT("interpolation"), MakeShared<FJsonValueNull>());
    }
    else
    {
        ResultObj->SetStringField(TEXT("interpolation"), InterpolationStr);
    }
    return MakeSuccessResult(ResultObj,
        FString::Printf(TEXT("Added keyframe at t=%.3fs on animation '%s'"), TimeSeconds, *AnimationName));
}

