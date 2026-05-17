// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_RemoveAnimationKeyframe.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSequenceEditHandlers.h"
#include "ClaireonWidgetAnimationHandlers.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Dom/JsonObject.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MovieScene.h"
#include "MovieSceneBinding.h"
#include "MovieSceneTrack.h"
#include "MovieSceneSection.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_RemoveAnimationKeyframe::GetOperation() const { return TEXT("remove_animation_keyframe"); }

FString ClaireonWidgetBPTool_RemoveAnimationKeyframe::GetDescription() const
{
    return TEXT("Remove a keyframe from a widget animation track.");
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
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult BeginError;
    if (!BeginSessionOp(Arguments, TEXT("remove_animation_keyframe"), Params, SessionId, Data, BeginError))
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
    double TimeSeconds = 0.0;
    Params->TryGetNumberField(TEXT("time"), TimeSeconds);
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
    UMovieScene* MS = Anim->GetMovieScene();
    if (!MS)
    {
        return MakeErrorResult(TEXT("animation has no MovieScene"));
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

    const FMovieSceneBinding* MSBinding = MS->GetBindings().FindByPredicate(
        [&](const FMovieSceneBinding& B) { return B.GetObjectGuid() == BindingGuid; });
    UMovieSceneTrack* Track = nullptr;
    if (MSBinding)
    {
        const FString Wanted = TrackName.IsEmpty() ? PropertyName : TrackName;
        const FName WantedFName(*Wanted);
        for (UMovieSceneTrack* T : MSBinding->GetTracks())
        {
            if (!T) { continue; }
            if (Wanted.IsEmpty() || T->GetTrackName() == WantedFName || T->GetClass()->GetName() == Wanted)
            {
                Track = T;
                break;
            }
        }
        if (!Track && MSBinding->GetTracks().Num() > 0)
        {
            Track = MSBinding->GetTracks()[0];
        }
    }
    if (!Track)
    {
        return MakeErrorResult(TEXT("no track resolved for remove_animation_keyframe"));
    }
    const TArray<UMovieSceneSection*>& Sections = Track->GetAllSections();
    if (Sections.Num() == 0 || !Sections[0])
    {
        return MakeErrorResult(TEXT("target track has no sections"));
    }
    UMovieSceneSection* Section = Sections[0];

    const FFrameRate TickResolution = MS->GetTickResolution();
    const FFrameNumber FrameNumber = (TimeSeconds * TickResolution).FloorToFrame();

    FScopedTransaction Transaction(NSLOCTEXT("Claireon", "RemoveWidgetAnimationKeyframe", "Remove Widget Animation Keyframe"));
    FString ApplyError;
    if (!Claireon::SequenceEdit::ApplyRemoveKeyframe(Section, FrameNumber, ApplyError))
    {
        Transaction.Cancel();
        return MakeErrorResult(ApplyError);
    }
    WBP->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    Data->bModified = true;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("removed"), true);
    ResultObj->SetNumberField(TEXT("frame"), FrameNumber.Value);
    return MakeSuccessResult(ResultObj,
        FString::Printf(TEXT("Removed keyframe at t=%.3fs from animation '%s'"), TimeSeconds, *AnimationName));
}

