// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCurveTool_Create.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonLog.h"
#include "ClaireonScopedAssetLock.h"
#include "ClaireonSessionManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Curves/CurveBase.h"
#include "Curves/CurveFloat.h"
#include "Curves/CurveLinearColor.h"
#include "Curves/CurveVector.h"
#include "Dom/JsonObject.h"
#include "Misc/PackageName.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "ClaireonCurveTool"

FString FClaireonCurveTool_Create::GetCategory() const { return TEXT("curve"); }
FString FClaireonCurveTool_Create::GetOperation() const { return TEXT("create"); }

TArray<FString> FClaireonCurveTool_Create::GetSearchKeywords() const
{
    return {TEXT("curve"), TEXT("create"), TEXT("new"), TEXT("float"), TEXT("vector"),
            TEXT("linear_color"), TEXT("ramp"), TEXT("asset"), TEXT("CurveFloat")};
}

FString FClaireonCurveTool_Create::GetDescription() const
{
    return TEXT("Create a standalone UCurveFloat, UCurveVector, or UCurveLinearColor asset at a /Game/ path "
                "(curve_type selects which). Errors if an asset already exists there. The new asset has no "
                "keys -- populate it with curve_add_key or curve_set_keys. Non-session and immediate: "
                "creates and saves the asset in one call, opening no editing session.");
}

TSharedPtr<FJsonObject> FClaireonCurveTool_Create::GetInputSchema() const
{
    FToolSchemaBuilder S;
    S.AddString(TEXT("asset_path"), TEXT("Destination /Game/ path for the new curve asset"), true);
    S.AddEnum(TEXT("curve_type"), TEXT("Curve asset type to create"),
        {TEXT("float"), TEXT("vector"), TEXT("linear_color")}, true);
    return S.Build();
}

IClaireonTool::FToolResult FClaireonCurveTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

    FString CurveType;
    if (!Arguments->TryGetStringField(TEXT("curve_type"), CurveType) || CurveType.IsEmpty())
    {
        return MakeErrorResult(TEXT("Missing required field: curve_type"));
    }

    UClass* ResolvedClass = nullptr;
    int32 ExpectedCurveCount = 0;
    if (CurveType.Equals(TEXT("float"), ESearchCase::IgnoreCase))
    {
        ResolvedClass = UCurveFloat::StaticClass();
        ExpectedCurveCount = 1;
    }
    else if (CurveType.Equals(TEXT("vector"), ESearchCase::IgnoreCase))
    {
        ResolvedClass = UCurveVector::StaticClass();
        ExpectedCurveCount = 3;
    }
    else if (CurveType.Equals(TEXT("linear_color"), ESearchCase::IgnoreCase))
    {
        ResolvedClass = UCurveLinearColor::StaticClass();
        ExpectedCurveCount = 4;
    }
    else
    {
        return MakeErrorResult(FString::Printf(
            TEXT("Unknown curve_type '%s'. Valid values: float, vector, linear_color."), *CurveType));
    }

    const FString Canon = FClaireonSessionManager::CanonicalizePath(AssetPath);
    if (Canon.IsEmpty())
    {
        return MakeErrorResult(TEXT("Invalid asset path (must start with /Game/)"));
    }

    // A resident-but-garbage object at this path would make LoadObject succeed while
    // the asset is effectively gone, so test IsValid rather than the raw pointer.
    if (UObject* Existing = LoadObject<UObject>(nullptr, *Canon); IsValid(Existing))
    {
        if (IsValid(Existing))
        {
            return MakeErrorResult(FString::Printf(TEXT("Asset already exists at path: %s"), *Canon));
        }
    }

    const FString ObjectName = FPackageName::GetShortName(Canon);

    FScopedTransaction Transaction(LOCTEXT("CreateCurve", "[Claireon] Create Curve"));

    UPackage* Package = CreatePackage(*Canon);
    if (!IsValid(Package))
    {
        Transaction.Cancel();
        return MakeErrorResult(FString::Printf(TEXT("CreatePackage failed for %s"), *Canon));
    }

    // NewObject, not the UCurveFactory classes: their FactoryCreateNew is this same
    // call plus editor class-picker machinery a headless tool neither needs nor
    // should depend on.
    UCurveBase* NewAsset = NewObject<UCurveBase>(
        Package, ResolvedClass, *ObjectName,
        RF_Public | RF_Standalone | RF_Transactional | RF_LoadCompleted);
    if (!IsValid(NewAsset))
    {
        Transaction.Cancel();
        return MakeErrorResult(FString::Printf(TEXT("NewObject failed for %s at %s"),
            *ResolvedClass->GetName(), *Canon));
    }

    FAssetRegistryModule::AssetCreated(NewAsset);
    Package->MarkPackageDirty();

    FString SaveError;
    if (!ClaireonAssetUtils::SaveAsset(NewAsset, SaveError))
    {
        Transaction.Cancel();
        NewAsset->ClearFlags(RF_Standalone | RF_Public);
        NewAsset->MarkAsGarbage();
        return MakeErrorResult(SaveError);
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetStringField(TEXT("asset_path"), NewAsset->GetPathName());
    Data->SetStringField(TEXT("curve_type"), CurveType);
    Data->SetNumberField(TEXT("num_curves"), ExpectedCurveCount);

    return MakeSuccessResult(Data, FString::Printf(
        TEXT("Created %s curve asset at %s (%d sub-curve(s))."),
        *CurveType, *NewAsset->GetPathName(), ExpectedCurveCount));
}

FString FClaireonCurveTool_Create::GetFullDescription() const
{
    return TEXT(
        "Creates a standalone curve asset so designers can tune a ramp in the curve "
        "editor instead of having its values inlined as pin-default magic numbers.\n\n"
        "curve_type selects the class and therefore the sub-curve layout:\n"
        "  float        -> UCurveFloat, 1 sub-curve (named after the asset itself)\n"
        "  vector       -> UCurveVector, 3 sub-curves named X, Y, Z\n"
        "  linear_color -> UCurveLinearColor, 4 sub-curves named R, G, B, A\n\n"
        "The asset is created empty and saved immediately, so it shows up in the "
        "content browser before any keys exist. Populate it with curve_set_keys (replace "
        "all) or curve_add_key (one at a time).\n\n"
        "An existing asset at the path is an error rather than an overwrite. Note that "
        "data_asset_create cannot make these: curves derive from UCurveBase, not "
        "UDataAsset.");
}

FString FClaireonCurveTool_Create::GetExampleUsage() const
{
    return TEXT("curve_create(asset_path='/Game/Curves/Curve_FireRate', curve_type='float')");
}

TSharedPtr<FJsonObject> FClaireonCurveTool_Create::GetParameterTooltips() const
{
    TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
    T->SetStringField(TEXT("asset_path"), TEXT("Destination /Game/ path. Must not already hold an asset."));
    T->SetStringField(TEXT("curve_type"), TEXT("'float' (1 curve), 'vector' (X/Y/Z), or 'linear_color' (R/G/B/A)."));
    return T;
}

#undef LOCTEXT_NAMESPACE
