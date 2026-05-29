// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_RemoveAnimationTrack.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonWidgetAnimationHandlers.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Dom/JsonObject.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MovieScene.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_RemoveAnimationTrack::GetOperation() const { return TEXT("remove_animation_track"); }

FString ClaireonWidgetBPTool_RemoveAnimationTrack::GetDescription() const
{
    return TEXT("Remove a track from a UWidgetAnimation in the open widget BP session. "
                "Session-mode tool: open via widgetbp_open first. Resolves the track on the binding identified "
                "by widget_name (or binding_guid) and a track_name/property_name (case-insensitive match against "
                "track name, then track class name).");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_RemoveAnimationTrack::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the UWidgetAnimation."), true);
    Builder.AddString(TEXT("widget_name"), TEXT("Widget whose binding owns the target track. Mutually exclusive with binding_guid."));
    Builder.AddString(TEXT("binding_guid"), TEXT("Animation binding GUID. Mutually exclusive with widget_name."));
    Builder.AddString(TEXT("track_name"), TEXT("Track identifier (track name or class name). property_name accepted as an alias."));
    Builder.AddString(TEXT("property_name"), TEXT("Alias for track_name."));
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_RemoveAnimationTrack::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("track"), TEXT("remove"), TEXT("sequence") };
}

FToolResult ClaireonWidgetBPTool_RemoveAnimationTrack::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult BeginError;
    if (!BeginSessionOp(Arguments, TEXT("remove_animation_track"), Params, SessionId, Data, BeginError))
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
    FString TrackNameOrProperty;
    if (!Params->TryGetStringField(TEXT("track_name"), TrackNameOrProperty))
    {
        Params->TryGetStringField(TEXT("property_name"), TrackNameOrProperty);
    }
    if (TrackNameOrProperty.IsEmpty())
    {
        return MakeErrorResult(TEXT("track_name (or property_name alias) is required"));
    }

    UWidgetAnimation* Anim = Claireon::WidgetAnimation::FindWidgetAnimationByName(WBP, AnimationName);
    if (!Anim)
    {
        return MakeErrorResult(FString::Printf(TEXT("animation '%s' not found on %s"), *AnimationName, *WBP->GetName()));
    }

    FGuid BindingGuid;
    FString BindingGuidStr;
    if (Params->TryGetStringField(TEXT("binding_guid"), BindingGuidStr) && !BindingGuidStr.IsEmpty())
    {
        if (!FGuid::Parse(BindingGuidStr, BindingGuid))
        {
            return MakeErrorResult(FString::Printf(TEXT("binding_guid '%s' is not a parseable GUID"), *BindingGuidStr));
        }
    }
    else
    {
        FString WidgetName;
        Params->TryGetStringField(TEXT("widget_name"), WidgetName);
        if (!WidgetName.IsEmpty())
        {
            const FName WFName(*WidgetName);
            for (const FWidgetAnimationBinding& B : Anim->GetBindings())
            {
                if (B.WidgetName == WFName)
                {
                    BindingGuid = B.AnimationGuid;
                    break;
                }
            }
        }
        else if (Anim->GetBindings().Num() > 0)
        {
            BindingGuid = Anim->GetBindings()[0].AnimationGuid;
        }
    }
    if (!BindingGuid.IsValid())
    {
        return MakeErrorResult(TEXT("no binding resolved; provide widget_name or binding_guid"));
    }

    FScopedTransaction Transaction(NSLOCTEXT("Claireon", "RemoveWidgetAnimationTrack", "Remove Widget Animation Track"));
    FString ApplyError;
    if (!Claireon::WidgetAnimation::ApplyRemoveAnimationTrack(Anim, BindingGuid, TrackNameOrProperty, ApplyError))
    {
        Transaction.Cancel();
        return MakeErrorResult(ApplyError);
    }
    WBP->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    Data->bModified = true;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetBoolField(TEXT("removed"), true);
    ResultObj->SetStringField(TEXT("track_name"), TrackNameOrProperty);
    ResultObj->SetStringField(TEXT("binding_guid"), BindingGuid.ToString(EGuidFormats::DigitsWithHyphens));
    return MakeSuccessResult(ResultObj,
        FString::Printf(TEXT("Removed track '%s' from animation '%s'"), *TrackNameOrProperty, *AnimationName));
}
