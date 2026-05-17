// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_DuplicateAnimation.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonWidgetAnimationHandlers.h"
#include "Animation/WidgetAnimation.h"
#include "Dom/JsonObject.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "MovieScene.h"
#include "ScopedTransaction.h"
#include "UObject/UObjectGlobals.h"
#include "WidgetBlueprint.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_DuplicateAnimation::GetOperation() const { return TEXT("duplicate_animation"); }

FString ClaireonWidgetBPTool_DuplicateAnimation::GetDescription() const
{
    return TEXT("Duplicate a UWidgetAnimation under a new name.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_DuplicateAnimation::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("animation_name"), TEXT("Name of the source animation to duplicate."), true);
    Builder.AddString(TEXT("new_name"), TEXT("Name of the duplicate."), true);
    return Builder.Build();
}

TArray<FString> ClaireonWidgetBPTool_DuplicateAnimation::GetSearchKeywords() const
{
    return { TEXT("UMG"), TEXT("widget"), TEXT("animation"), TEXT("duplicate"), TEXT("copy"), TEXT("sequence") };
}

FToolResult ClaireonWidgetBPTool_DuplicateAnimation::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult BeginError;
    if (!BeginSessionOp(Arguments, TEXT("duplicate_animation"), Params, SessionId, Data, BeginError))
    {
        return BeginError;
    }
    UWidgetBlueprint* WBP = Data ? Data->WidgetBlueprint.Get() : nullptr;
    if (!WBP)
    {
        return MakeErrorResult(TEXT("widget blueprint unavailable on session"));
    }
    FString SourceName;
    if (!Params->TryGetStringField(TEXT("animation_name"), SourceName) || SourceName.IsEmpty())
    {
        return MakeErrorResult(TEXT("animation_name is required"));
    }
    FString NewName;
    if (!Params->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
    {
        return MakeErrorResult(TEXT("new_name is required"));
    }

    UWidgetAnimation* Source = Claireon::WidgetAnimation::FindWidgetAnimationByName(WBP, SourceName);
    if (!Source)
    {
        return MakeErrorResult(FString::Printf(TEXT("source animation '%s' not found on %s"), *SourceName, *WBP->GetName()));
    }

    FScopedTransaction Transaction(NSLOCTEXT("Claireon", "DuplicateWidgetAnimation", "Duplicate Widget Animation"));

    const FName UniqueFName = MakeUniqueObjectName(WBP, UWidgetAnimation::StaticClass(), FName(*NewName));
    UWidgetAnimation* Dup = DuplicateObject<UWidgetAnimation>(Source, WBP, UniqueFName);
    if (!Dup)
    {
        Transaction.Cancel();
        return MakeErrorResult(TEXT("DuplicateObject returned null"));
    }
    // Engine-critical: the duplicated MovieScene keeps the source's name unless we
    // rename it to match the new animation's name (AnimationTabSummoner
    // OnDuplicateAnimation pattern).
    if (Dup->MovieScene)
    {
        Dup->MovieScene->Rename(*Dup->GetName(), nullptr, REN_DontCreateRedirectors);
    }
    Dup->SetDisplayLabel(Dup->GetName());

    WBP->Modify();
    WBP->Animations.Add(Dup);
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
    Data->bModified = true;

    TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
    ResultObj->SetStringField(TEXT("name"), Dup->GetFName().ToString());
    ResultObj->SetStringField(TEXT("source"), SourceName);

    return MakeSuccessResult(ResultObj,
        FString::Printf(TEXT("Duplicated '%s' -> '%s'"), *SourceName, *Dup->GetFName().ToString()));
}

