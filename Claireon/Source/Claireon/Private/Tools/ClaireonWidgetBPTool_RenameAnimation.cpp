// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_RenameAnimation.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonWidgetAnimationHandlers.h"
#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_RenameAnimation::GetOperation() const { return TEXT("rename_animation"); }

FString ClaireonWidgetBPTool_RenameAnimation::GetDescription() const
{
    return TEXT("Rename a UWidgetAnimation on the Widget Blueprint to a new name. Session-mode tool: open via widgetbp_open first.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_RenameAnimation::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Current animation name."), true);
    Builder.AddString(TEXT("new_name"), TEXT("New animation name."), true);
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_RenameAnimation::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("rename"), TEXT("sequence") };
}

FToolResult ClaireonWidgetBPTool_RenameAnimation::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult BeginError;
    if (!BeginSessionOp(Arguments, TEXT("rename_animation"), Params, SessionId, Data, BeginError))
    {
        return BeginError;
    }
    UWidgetBlueprint* WBP = Data ? Data->WidgetBlueprint.Get() : nullptr;
    if (!WBP)
    {
        return MakeErrorResult(TEXT("widget blueprint unavailable on session"));
    }
    FString OldName;
    if (!Params->TryGetStringField(TEXT("animation_name"), OldName) || OldName.IsEmpty())
    {
        return MakeErrorResult(TEXT("animation_name is required"));
    }
    FString NewName;
    if (!Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
    {
        return MakeErrorResult(TEXT("new_name is required"));
    }

    FScopedTransaction Transaction(NSLOCTEXT("Claireon", "RenameWidgetAnimation", "Rename Widget Animation"));
    FString ApplyError;
    if (!Claireon::WidgetAnimation::ApplyRenameAnimation(WBP, OldName, NewName, ApplyError))
    {
        Transaction.Cancel();
        return MakeErrorResult(ApplyError);
    }
    Data->bModified = true;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("old_name"), OldName);
    ResultObj->SetStringField(TEXT("new_name"), NewName);
    return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Renamed '%s' -> '%s'"), *OldName, *NewName));
}

