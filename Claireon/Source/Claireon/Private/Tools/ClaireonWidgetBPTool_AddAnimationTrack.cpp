// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_AddAnimationTrack.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonWidgetAnimationHandlers.h"
#include "Animation/WidgetAnimation.h"
#include "Animation/WidgetAnimationBinding.h"
#include "Dom/JsonObject.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MovieSceneTrack.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_AddAnimationTrack::GetName() const
{
    return TEXT("claireon.widgetbp_add_animation_track");
}

FString ClaireonWidgetBPTool_AddAnimationTrack::GetDescription() const
{
    return TEXT("Add a track to a UWidgetAnimation.");
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
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult BeginError;
    if (!BeginSessionOp(Arguments, TEXT("add_animation_track"), Params, SessionId, Data, BeginError))
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
    FString WidgetName;
    if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName) || WidgetName.IsEmpty())
    {
        return MakeErrorResult(TEXT("widget_name is required"));
    }
    FString PropertyName;
    if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
    {
        // Accept legacy "property_path" alias used by existing spec test.
        Params->TryGetStringField(TEXT("property_path"), PropertyName);
    }
    FString TrackType;
    Params->TryGetStringField(TEXT("track_type"), TrackType);

    UWidgetAnimation* Anim = FindWidgetAnimationByName(WBP, AnimationName);
    if (!Anim)
    {
        return MakeErrorResult(FString::Printf(TEXT("animation '%s' not found on %s"), *AnimationName, *WBP->GetName()));
    }

    const FName WidgetFName(*WidgetName);
    FGuid BindingGuid;
    for (const FWidgetAnimationBinding& B : Anim->GetBindings())
    {
        if (B.WidgetName == WidgetFName)
        {
            BindingGuid = B.AnimationGuid;
            break;
        }
    }
    if (!BindingGuid.IsValid())
    {
        return MakeErrorResult(FString::Printf(TEXT("widget '%s' is not bound to animation '%s'; call add_animation_binding first"), *WidgetName, *AnimationName));
    }

    FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AddWidgetAnimationTrack", "Add Widget Animation Track"));
    UMovieSceneTrack* NewTrack = nullptr;
    FString ApplyError;
    if (!ApplyAddAnimationTrack(Anim, BindingGuid, TrackType, PropertyName, NewTrack, ApplyError))
    {
        Transaction.Cancel();
        return MakeErrorResult(ApplyError);
    }
    WBP->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    Data->bModified = true;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("track_name"), NewTrack->GetTrackName().ToString());
    ResultObj->SetStringField(TEXT("track_class"), NewTrack->GetClass()->GetName());
    ResultObj->SetNumberField(TEXT("section_index"), 0);
    return MakeSuccessResult(ResultObj,
        FString::Printf(TEXT("Added %s track to animation '%s'"), *NewTrack->GetClass()->GetName(), *AnimationName));
}

