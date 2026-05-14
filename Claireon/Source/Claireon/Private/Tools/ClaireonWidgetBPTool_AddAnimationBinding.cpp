// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_AddAnimationBinding.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonWidgetAnimationHandlers.h"
#include "ClaireonWidgetHelpers.h"
#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Dom/JsonObject.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_AddAnimationBinding::GetOperation() const { return TEXT("add_animation_binding"); }

FString ClaireonWidgetBPTool_AddAnimationBinding::GetDescription() const
{
    return TEXT("Bind a widget to a UWidgetAnimation (UWidgetAnimationBinding).");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_AddAnimationBinding::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the UWidgetAnimation."), true);
    Builder.AddString(TEXT("widget_name"), TEXT("Widget in the tree to bind."), true);
    Builder.AddString(TEXT("slot_widget_name"), TEXT("Optional slot-wrapper widget name; when set, binds at slot level (Canvas slot, HBox slot, etc.) via FWidgetAnimationBinding::SlotWidgetName."));
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_AddAnimationBinding::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("binding"), TEXT("bind"), TEXT("sequence") };
}

FToolResult ClaireonWidgetBPTool_AddAnimationBinding::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult BeginError;
    if (!BeginSessionOp(Arguments, TEXT("add_animation_binding"), Params, SessionId, Data, BeginError))
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
    FString SlotWidgetName;
    Params->TryGetStringField(TEXT("slot_widget_name"), SlotWidgetName);

    UWidgetAnimation* Anim = FindWidgetAnimationByName(WBP, AnimationName);
    if (!Anim)
    {
        return MakeErrorResult(FString::Printf(TEXT("animation '%s' not found on %s"), *AnimationName, *WBP->GetName()));
    }
    UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetName));
    if (!Widget)
    {
        return MakeErrorResult(FString::Printf(TEXT("widget '%s' not found on %s"), *WidgetName, *WBP->GetName()));
    }

    FScopedTransaction Transaction(NSLOCTEXT("Claireon", "AddWidgetAnimationBinding", "Add Widget Animation Binding"));
    FGuid NewGuid;
    FString ApplyError;
    if (!ApplyAddAnimationBinding(Anim, Widget, SlotWidgetName, NewGuid, ApplyError))
    {
        Transaction.Cancel();
        return MakeErrorResult(ApplyError);
    }
    WBP->Modify();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    Data->bModified = true;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("guid"), NewGuid.ToString(EGuidFormats::DigitsWithHyphens));
    ResultObj->SetStringField(TEXT("widget_name"), Widget->GetName());
    if (SlotWidgetName.IsEmpty())
    {
        ResultObj->SetField(TEXT("slot_widget_name"), MakeShared<FJsonValueNull>());
    }
    else
    {
        ResultObj->SetStringField(TEXT("slot_widget_name"), SlotWidgetName);
    }
    return MakeSuccessResult(ResultObj,
        FString::Printf(TEXT("Bound widget '%s' to animation '%s'"), *Widget->GetName(), *AnimationName));
}

