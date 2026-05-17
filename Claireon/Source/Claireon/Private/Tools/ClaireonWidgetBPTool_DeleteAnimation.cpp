// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_DeleteAnimation.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonWidgetAnimationHandlers.h"
#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"
#include "WidgetBlueprint.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_DeleteAnimation::GetOperation() const { return TEXT("delete_animation"); }

FString ClaireonWidgetBPTool_DeleteAnimation::GetDescription() const
{
    return TEXT("Delete a UWidgetAnimation by name.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_DeleteAnimation::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the UWidgetAnimation to delete."), true);
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_DeleteAnimation::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("delete"), TEXT("remove"), TEXT("sequence") };
}

FToolResult ClaireonWidgetBPTool_DeleteAnimation::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult BeginError;
    if (!BeginSessionOp(Arguments, TEXT("delete_animation"), Params, SessionId, Data, BeginError))
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

    FScopedTransaction Transaction(NSLOCTEXT("Claireon", "DeleteWidgetAnimation", "Delete Widget Animation"));
    FString ApplyError;
    if (!Claireon::WidgetAnimation::ApplyDeleteAnimation(WBP, AnimationName, ApplyError))
    {
        Transaction.Cancel();
        return MakeErrorResult(ApplyError);
    }
    Data->bModified = true;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("deleted"), AnimationName);
    return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Deleted animation '%s'"), *AnimationName));
}

