// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCurveTool_ClearKeys.h"
#include "Tools/ClaireonCurveToolHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonLog.h"
#include "ClaireonScopedAssetLock.h"
#include "Curves/CurveBase.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "ClaireonCurveTool"

FString FClaireonCurveTool_ClearKeys::GetCategory() const { return TEXT("curve"); }
FString FClaireonCurveTool_ClearKeys::GetOperation() const { return TEXT("clear_keys"); }

TArray<FString> FClaireonCurveTool_ClearKeys::GetSearchKeywords() const
{
    return {TEXT("curve"), TEXT("clear"), TEXT("keys"), TEXT("reset"), TEXT("empty"),
            TEXT("remove"), TEXT("wipe")};
}

FString FClaireonCurveTool_ClearKeys::GetDescription() const
{
    return TEXT("Remove all keys from one sub-curve of a curve asset (selected by curve_index or curve_name) "
                "or, if neither selector is given, from every sub-curve on the asset, then save. "
                "Non-session and immediate: loads, clears, and saves in one call -- no open curve session "
                "is involved.");
}

TSharedPtr<FJsonObject> FClaireonCurveTool_ClearKeys::GetInputSchema() const
{
    FToolSchemaBuilder S;
    S.AddString(TEXT("asset_path"), TEXT("Path to an existing curve asset"), true);
    S.AddInteger(TEXT("curve_index"), TEXT("0-based sub-curve index to clear. Omit together with curve_name to clear every sub-curve on the asset."), false);
    S.AddString(TEXT("curve_name"), TEXT("Sub-curve name selector. Omit together with curve_index to clear every sub-curve on the asset. Not valid for float assets."), false);
    return S.Build();
}

IClaireonTool::FToolResult FClaireonCurveTool_ClearKeys::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    FString AssetPath;
    if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
    {
        return MakeErrorResult(TEXT("Missing required field: asset_path"));
    }

    FClaireonScopedAssetLock Lock(AssetPath, GetName());
    if (!Lock.IsAcquired())
    {
        return Lock.GetError();
    }

    FString LoadError;
    UCurveBase* Curve = ClaireonCurveToolHelpers::LoadCurveAsset(AssetPath, LoadError);
    if (!IsValid(Curve))
    {
        return MakeErrorResult(LoadError);
    }

    // No selector at all means "clear the whole asset", which is why this tool passes
    // bAllowSelectAll unlike add_key / set_keys.
    TArray<FRichCurveEditInfo> Selected;
    FString SelectError;
    if (!ClaireonCurveToolHelpers::ResolveSubCurves(Curve, Arguments, /*bAllowSelectAll=*/true, Selected, SelectError))
    {
        return MakeErrorResult(SelectError);
    }
    if (Selected.Num() == 0)
    {
        return MakeErrorResult(TEXT("No sub-curves resolved to clear"));
    }

    FScopedTransaction Transaction(LOCTEXT("CurveClearKeys", "[Claireon] Clear Curve Keys"));
    Curve->Modify();

    TArray<TSharedPtr<FJsonValue>> ClearedNames;
    for (const FRichCurveEditInfo& Entry : Selected)
    {
        if (!Entry.CurveToEdit) { continue; }
        Entry.CurveToEdit->Reset();
        ClearedNames.Add(MakeShared<FJsonValueString>(Entry.CurveName.ToString()));
    }

    FString SaveError;
    if (!ClaireonAssetUtils::SaveAsset(Curve, SaveError))
    {
        return MakeErrorResult(SaveError);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("cleared_curve_names"), ClearedNames);

    return MakeSuccessResult(Data, FString::Printf(
        TEXT("Cleared %d sub-curve(s) on %s."), ClearedNames.Num(), *Curve->GetName()));
}

FString FClaireonCurveTool_ClearKeys::GetFullDescription() const
{
    return TEXT(
        "Empties curve keys without deleting the asset, so a ramp can be re-authored "
        "from scratch while every reference to the curve stays intact.\n\n"
        "Scope depends on the selector:\n"
        "  curve_index or curve_name given -> that one sub-curve is reset\n"
        "  both omitted                    -> EVERY sub-curve on the asset is reset\n\n"
        "That whole-asset default is specific to this tool; curve_add_key and "
        "curve_set_keys treat an omitted selector as index 0 instead.\n\n"
        "Response cleared_curve_names lists exactly what was touched -- 'X','Y','Z' or "
        "'R','G','B','A' for the container types, and the asset's own name for a float "
        "curve, whose single sub-curve is named after the asset.\n\n"
        "Use curve_set_keys with a new list to replace keys in one step rather than "
        "clearing and then adding.");
}

FString FClaireonCurveTool_ClearKeys::GetExampleUsage() const
{
    return TEXT("curve_clear_keys(asset_path='/Game/Curves/Curve_Tint', curve_name='G')");
}

TSharedPtr<FJsonObject> FClaireonCurveTool_ClearKeys::GetParameterTooltips() const
{
    TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
    T->SetStringField(TEXT("asset_path"), TEXT("Path to an existing UCurveFloat / UCurveVector / UCurveLinearColor."));
    T->SetStringField(TEXT("curve_index"), TEXT("Sub-curve index to clear. Omit with curve_name to clear all of them."));
    T->SetStringField(TEXT("curve_name"), TEXT("Sub-curve name to clear. Omit with curve_index to clear all of them."));
    return T;
}

#undef LOCTEXT_NAMESPACE
