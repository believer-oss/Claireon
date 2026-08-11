// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCurveTool_AddKey.h"
#include "Tools/ClaireonCurveToolHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonLog.h"
#include "ClaireonScopedAssetLock.h"
#include "Curves/CurveBase.h"
#include "Dom/JsonObject.h"
#include "ScopedTransaction.h"

#define LOCTEXT_NAMESPACE "ClaireonCurveTool"

FString FClaireonCurveTool_AddKey::GetCategory() const { return TEXT("curve"); }
FString FClaireonCurveTool_AddKey::GetOperation() const { return TEXT("add_key"); }

TArray<FString> FClaireonCurveTool_AddKey::GetSearchKeywords() const
{
    return {TEXT("curve"), TEXT("add"), TEXT("key"), TEXT("keyframe"), TEXT("point"),
            TEXT("time"), TEXT("value"), TEXT("interp"), TEXT("ramp")};
}

FString FClaireonCurveTool_AddKey::GetDescription() const
{
    return TEXT("Add one key (time, value, optional interp_mode) to a curve asset and save it. For "
                "UCurveVector/UCurveLinearColor, pick the sub-curve with curve_index or curve_name; "
                "defaults to index 0. Non-session and immediate: loads the asset, edits it inside an "
                "undoable transaction, and saves in one call -- no open curve session is involved.");
}

TSharedPtr<FJsonObject> FClaireonCurveTool_AddKey::GetInputSchema() const
{
    FToolSchemaBuilder S;
    S.AddString(TEXT("asset_path"), TEXT("Path to an existing curve asset"), true);
    S.AddNumber(TEXT("time"), TEXT("Key time in seconds"), true);
    S.AddNumber(TEXT("value"), TEXT("Key value"), true);
    S.AddEnum(TEXT("interp_mode"), TEXT("Interpolation mode for this key (default 'cubic')"),
        {TEXT("linear"), TEXT("cubic"), TEXT("constant")}, false);
    S.AddInteger(TEXT("curve_index"), TEXT("0-based sub-curve index for vector (0-2) / linear_color (0-3) assets. Default 0."), false);
    S.AddString(TEXT("curve_name"), TEXT("Sub-curve name: X/Y/Z for vector, R/G/B/A for linear_color. Not valid for float assets. Overrides curve_index if both given."), false);
    return S.Build();
}

IClaireonTool::FToolResult FClaireonCurveTool_AddKey::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

    double Time = 0.0;
    if (!Arguments->TryGetNumberField(TEXT("time"), Time))
    {
        return MakeErrorResult(TEXT("Missing required field: time"));
    }
    double Value = 0.0;
    if (!Arguments->TryGetNumberField(TEXT("value"), Value))
    {
        return MakeErrorResult(TEXT("Missing required field: value"));
    }

    FString InterpModeString;
    Arguments->TryGetStringField(TEXT("interp_mode"), InterpModeString);
    const ERichCurveInterpMode InterpMode = ClaireonCurveToolHelpers::ParseInterpMode(InterpModeString);

    FString LoadError;
    UCurveBase* Curve = ClaireonCurveToolHelpers::LoadCurveAsset(AssetPath, LoadError);
    if (!IsValid(Curve))
    {
        return MakeErrorResult(LoadError);
    }

    TArray<FRichCurveEditInfo> Selected;
    FString SelectError;
    if (!ClaireonCurveToolHelpers::ResolveSubCurves(Curve, Arguments, /*bAllowSelectAll=*/false, Selected, SelectError))
    {
        return MakeErrorResult(SelectError);
    }
    if (Selected.Num() != 1 || Selected[0].CurveToEdit == nullptr)
    {
        return MakeErrorResult(TEXT("Failed to resolve a single target sub-curve"));
    }

    FScopedTransaction Transaction(LOCTEXT("CurveAddKey", "[Claireon] Add Curve Key"));
    Curve->Modify();

    FRealCurve* Target = Selected[0].CurveToEdit;
    const FKeyHandle Handle = Target->AddKey(static_cast<float>(Time), static_cast<float>(Value));
    Target->SetKeyInterpMode(Handle, InterpMode);

    FString SaveError;
    if (!ClaireonAssetUtils::SaveAsset(Curve, SaveError))
    {
        return MakeErrorResult(SaveError);
    }

    const int32 KeyCount = Target->GetNumKeys();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    // Rich curves keep keys time-sorted, so this is count-minus-one, not the
    // ordinal position the caller happened to add in.
    Data->SetNumberField(TEXT("key_index"), KeyCount - 1);
    Data->SetNumberField(TEXT("key_count"), KeyCount);
    Data->SetStringField(TEXT("curve_name"), Selected[0].CurveName.ToString());
    Data->SetStringField(TEXT("interp_mode"), ClaireonCurveToolHelpers::InterpModeToString(InterpMode));

    return MakeSuccessResult(Data, FString::Printf(
        TEXT("Added key (t=%g, v=%g, %s) to '%s'; %d key(s) total."),
        Time, Value, ClaireonCurveToolHelpers::InterpModeToString(InterpMode),
        *Selected[0].CurveName.ToString(), KeyCount));
}

FString FClaireonCurveTool_AddKey::GetFullDescription() const
{
    return TEXT(
        "Adds a single key to one sub-curve of an existing curve asset and saves it, so "
        "the change is visible to a designer immediately.\n\n"
        "Sub-curve selection: curve_name ('X'/'Y'/'Z' on a vector, 'R'/'G'/'B'/'A' on a "
        "linear color) takes precedence over curve_index; omitting both targets index 0. "
        "curve_name is rejected on a float asset because that asset's single curve is "
        "named after the asset itself and so has no stable token to address it by -- use "
        "curve_index=0 or omit the selector there.\n\n"
        "interp_mode is per-key and defaults to 'cubic'. A bad selector reports the valid "
        "indices or names for the loaded asset's actual type rather than a generic error.\n\n"
        "Response key_index is the key count minus one, not an insertion ordinal: rich "
        "curves keep keys sorted by time, so a key added out of order lands earlier in "
        "the sequence. Use curve_set_keys to replace a whole curve in one call.");
}

FString FClaireonCurveTool_AddKey::GetExampleUsage() const
{
    return TEXT("curve_add_key(asset_path='/Game/Curves/Curve_FireRate', time=0.0, value=1.0, interp_mode='linear')");
}

TSharedPtr<FJsonObject> FClaireonCurveTool_AddKey::GetParameterTooltips() const
{
    TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
    T->SetStringField(TEXT("asset_path"), TEXT("Path to an existing UCurveFloat / UCurveVector / UCurveLinearColor."));
    T->SetStringField(TEXT("time"), TEXT("Key time on the horizontal axis, in seconds."));
    T->SetStringField(TEXT("value"), TEXT("Key value on the vertical axis."));
    T->SetStringField(TEXT("interp_mode"), TEXT("'linear' | 'cubic' | 'constant'. Default 'cubic'."));
    T->SetStringField(TEXT("curve_index"), TEXT("Sub-curve index. Default 0; the only legal value on a float asset."));
    T->SetStringField(TEXT("curve_name"), TEXT("Sub-curve name. Wins over curve_index. Not valid on float assets."));
    return T;
}

#undef LOCTEXT_NAMESPACE
