// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCurveTool_SetKeys.h"
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

FString FClaireonCurveTool_SetKeys::GetCategory() const { return TEXT("curve"); }
FString FClaireonCurveTool_SetKeys::GetOperation() const { return TEXT("set_keys"); }

TArray<FString> FClaireonCurveTool_SetKeys::GetSearchKeywords() const
{
    return {TEXT("curve"), TEXT("set"), TEXT("keys"), TEXT("replace"), TEXT("ramp"),
            TEXT("keyframes"), TEXT("bulk"), TEXT("populate")};
}

FString FClaireonCurveTool_SetKeys::GetDescription() const
{
    return TEXT("Replace all keys on one sub-curve of a curve asset with the keys array ([time, value] tuples "
                "or {time, value, interp_mode} objects), then save. Existing keys on that sub-curve are "
                "discarded first (FRealCurve::Reset); other sub-curves are untouched. Non-session and "
                "immediate: loads, rewrites, and saves in one call, opening no editing session.");
}

TSharedPtr<FJsonObject> FClaireonCurveTool_SetKeys::GetInputSchema() const
{
    FToolSchemaBuilder S;
    S.AddString(TEXT("asset_path"), TEXT("Path to an existing curve asset"), true);
    S.AddArray(TEXT("keys"), TEXT("Replace-all key list. Each element is either [time, value] or {\"time\":t,\"value\":v,\"interp_mode\":\"linear\"|\"cubic\"|\"constant\"}. Tuple-form elements use the top-level interp_mode default."), true);
    S.AddEnum(TEXT("interp_mode"), TEXT("Default interpolation mode applied to tuple-form keys that omit their own interp_mode (default 'cubic')"),
        {TEXT("linear"), TEXT("cubic"), TEXT("constant")}, false);
    S.AddInteger(TEXT("curve_index"), TEXT("0-based sub-curve index. Default 0."), false);
    S.AddString(TEXT("curve_name"), TEXT("Sub-curve name selector. Overrides curve_index if both given. Not valid for float assets."), false);
    return S.Build();
}

IClaireonTool::FToolResult FClaireonCurveTool_SetKeys::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

    const TArray<TSharedPtr<FJsonValue>>* KeysArray = nullptr;
    if (!Arguments->TryGetArrayField(TEXT("keys"), KeysArray) || !KeysArray)
    {
        return MakeErrorResult(TEXT("Missing required field: keys"));
    }

    FString DefaultInterpString;
    Arguments->TryGetStringField(TEXT("interp_mode"), DefaultInterpString);
    const ERichCurveInterpMode DefaultInterp = ClaireonCurveToolHelpers::ParseInterpMode(DefaultInterpString);

    // Parse every entry before touching the curve, so a malformed list never leaves a
    // half-replaced curve behind (Reset has already run by then).
    struct FPendingKey
    {
        float Time = 0.0f;
        float Value = 0.0f;
        ERichCurveInterpMode Interp = RCIM_Cubic;
    };
    TArray<FPendingKey> PendingKeys;
    PendingKeys.Reserve(KeysArray->Num());

    for (int32 Index = 0; Index < KeysArray->Num(); ++Index)
    {
        const TSharedPtr<FJsonValue>& Element = (*KeysArray)[Index];
        if (!Element.IsValid())
        {
            return MakeErrorResult(FString::Printf(
                TEXT("keys[%d] must be a [time, value] array or a {time, value, interp_mode} object"), Index));
        }

        const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
        if (Element->TryGetArray(Tuple) && Tuple)
        {
            if (Tuple->Num() != 2)
            {
                return MakeErrorResult(FString::Printf(
                    TEXT("keys[%d] must be a 2-element [time, value] array"), Index));
            }
            double TupleTime = 0.0;
            double TupleValue = 0.0;
            if (!(*Tuple)[0]->TryGetNumber(TupleTime) || !(*Tuple)[1]->TryGetNumber(TupleValue))
            {
                return MakeErrorResult(FString::Printf(
                    TEXT("keys[%d] must be a 2-element [time, value] array"), Index));
            }
            PendingKeys.Add({static_cast<float>(TupleTime), static_cast<float>(TupleValue), DefaultInterp});
            continue;
        }

        const TSharedPtr<FJsonObject>* EntryObj = nullptr;
        if (Element->TryGetObject(EntryObj) && EntryObj && (*EntryObj).IsValid())
        {
            double EntryTime = 0.0;
            double EntryValue = 0.0;
            if (!(*EntryObj)->TryGetNumberField(TEXT("time"), EntryTime))
            {
                return MakeErrorResult(FString::Printf(TEXT("keys[%d] is missing required field 'time'"), Index));
            }
            if (!(*EntryObj)->TryGetNumberField(TEXT("value"), EntryValue))
            {
                return MakeErrorResult(FString::Printf(TEXT("keys[%d] is missing required field 'value'"), Index));
            }
            ERichCurveInterpMode EntryInterp = DefaultInterp;
            FString EntryInterpString;
            if ((*EntryObj)->TryGetStringField(TEXT("interp_mode"), EntryInterpString) && !EntryInterpString.IsEmpty())
            {
                EntryInterp = ClaireonCurveToolHelpers::ParseInterpMode(EntryInterpString);
            }
            PendingKeys.Add({static_cast<float>(EntryTime), static_cast<float>(EntryValue), EntryInterp});
            continue;
        }

        return MakeErrorResult(FString::Printf(
            TEXT("keys[%d] must be a [time, value] array or a {time, value, interp_mode} object"), Index));
    }

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

    FScopedTransaction Transaction(LOCTEXT("CurveSetKeys", "[Claireon] Set Curve Keys"));
    Curve->Modify();

    FRealCurve* Target = Selected[0].CurveToEdit;
    Target->Reset();
    for (const FPendingKey& Key : PendingKeys)
    {
        const FKeyHandle Handle = Target->AddKey(Key.Time, Key.Value);
        Target->SetKeyInterpMode(Handle, Key.Interp);
    }

    FString SaveError;
    if (!ClaireonAssetUtils::SaveAsset(Curve, SaveError))
    {
        return MakeErrorResult(SaveError);
    }

    const int32 KeyCount = Target->GetNumKeys();

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetNumberField(TEXT("key_count"), KeyCount);
    Data->SetStringField(TEXT("curve_name"), Selected[0].CurveName.ToString());

    return MakeSuccessResult(Data, FString::Printf(
        TEXT("Replaced keys on '%s' with %d key(s)."), *Selected[0].CurveName.ToString(), KeyCount));
}

FString FClaireonCurveTool_SetKeys::GetFullDescription() const
{
    return TEXT(
        "Replaces one sub-curve's entire key list in a single call, which is the right "
        "tool for authoring a whole ramp (a fire-rate curve, a damage falloff) rather "
        "than adding keys one at a time.\n\n"
        "keys accepts two element forms, mixable in one call:\n"
        "  [time, value]                                  -- uses the top-level interp_mode\n"
        "  {time, value, interp_mode}                      -- per-key override\n\n"
        "Order does not matter: rich curves sort by time on insert. The whole list is "
        "parsed and validated BEFORE the curve is reset, so a malformed element leaves "
        "the existing keys intact instead of wiping them and then failing.\n\n"
        "Only the selected sub-curve is reset -- other sub-curves on a vector or linear "
        "color asset keep their keys. Selection rules match curve_add_key: curve_name "
        "beats curve_index, both omitted means index 0, and curve_name is invalid on a "
        "float asset.");
}

FString FClaireonCurveTool_SetKeys::GetExampleUsage() const
{
    return TEXT("curve_set_keys(asset_path='/Game/Curves/Curve_FireRate', keys=[[0,1.0],[2,0.8],[4,0.6],[6,0.45],[8,0.3]], interp_mode='linear')");
}

TSharedPtr<FJsonObject> FClaireonCurveTool_SetKeys::GetParameterTooltips() const
{
    TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
    T->SetStringField(TEXT("asset_path"), TEXT("Path to an existing UCurveFloat / UCurveVector / UCurveLinearColor."));
    T->SetStringField(TEXT("keys"), TEXT("Replace-all list of [time, value] pairs or {time, value, interp_mode} objects."));
    T->SetStringField(TEXT("interp_mode"), TEXT("Default mode for entries that do not specify their own. Default 'cubic'."));
    T->SetStringField(TEXT("curve_index"), TEXT("Sub-curve index. Default 0; the only legal value on a float asset."));
    T->SetStringField(TEXT("curve_name"), TEXT("Sub-curve name. Wins over curve_index. Not valid on float assets."));
    return T;
}

#undef LOCTEXT_NAMESPACE
