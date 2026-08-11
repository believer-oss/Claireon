// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonCurveToolHelpers.h"
#include "ClaireonPathResolver.h"
#include "Curves/CurveBase.h"
#include "Dom/JsonObject.h"
#include "UObject/SoftObjectPath.h"

namespace ClaireonCurveToolHelpers
{
	ERichCurveInterpMode ParseInterpMode(const FString& ModeString)
	{
		if (ModeString.Equals(TEXT("linear"), ESearchCase::IgnoreCase))   { return RCIM_Linear; }
		if (ModeString.Equals(TEXT("constant"), ESearchCase::IgnoreCase)) { return RCIM_Constant; }
		// Cubic is the default, and RCIM_None is never emitted by these tools.
		return RCIM_Cubic;
	}

	const TCHAR* InterpModeToString(ERichCurveInterpMode Mode)
	{
		switch (Mode)
		{
		case RCIM_Linear:   return TEXT("linear");
		case RCIM_Constant: return TEXT("constant");
		case RCIM_Cubic:    return TEXT("cubic");
		default:            return TEXT("none");
		}
	}

	UCurveBase* LoadCurveAsset(const FString& AssetPath, FString& OutError)
	{
		ClaireonPathResolver::FResolveResult ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
		if (!ResolveResult.bSuccess)
		{
			OutError = ResolveResult.Error;
			return nullptr;
		}

		UObject* Loaded = FSoftObjectPath(ResolveResult.ResolvedPath.Path).TryLoad();
		if (!IsValid(Loaded))
		{
			OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *AssetPath);
			return nullptr;
		}

		UCurveBase* Curve = Cast<UCurveBase>(Loaded);
		if (!IsValid(Curve))
		{
			OutError = FString::Printf(TEXT("Asset at %s is not a curve asset (actual type: %s)"),
				*AssetPath, *Loaded->GetClass()->GetName());
			return nullptr;
		}
		return Curve;
	}

	bool ResolveSubCurves(
		UCurveBase* Curve,
		const TSharedPtr<FJsonObject>& Arguments,
		bool bAllowSelectAll,
		TArray<FRichCurveEditInfo>& OutSelected,
		FString& OutError)
	{
		OutSelected.Reset();
		if (!IsValid(Curve))
		{
			OutError = TEXT("Curve asset is null");
			return false;
		}

		// GetCurves() returns FRichCurveEditInfo whose CurveToEdit is FRealCurve*, not
		// FRichCurve* -- the mutators are virtual on the base, so no cast is needed.
		TArray<FRichCurveEditInfo> Curves = Curve->GetCurves();
		if (Curves.Num() == 0)
		{
			OutError = FString::Printf(TEXT("Curve asset %s exposes no sub-curves"), *Curve->GetName());
			return false;
		}

		TArray<FString> ValidNames;
		ValidNames.Reserve(Curves.Num());
		for (const FRichCurveEditInfo& Entry : Curves)
		{
			ValidNames.Add(Entry.CurveName.ToString());
		}

		FString CurveName;
		const bool bHasCurveName =
			Arguments.IsValid() && Arguments->TryGetStringField(TEXT("curve_name"), CurveName) && !CurveName.IsEmpty();
		const bool bHasCurveIndex = Arguments.IsValid() && Arguments->HasField(TEXT("curve_index"));

		if (!bHasCurveName && !bHasCurveIndex && bAllowSelectAll)
		{
			OutSelected = Curves;
			return true;
		}

		if (bHasCurveName)
		{
			// A float asset's single curve is named after the asset itself, so there is
			// no stable token to address it by -- the selector is meaningless there.
			if (Curves.Num() == 1)
			{
				OutError = TEXT("curve_name is not supported for float curve assets; use curve_index=0 or omit the selector.");
				return false;
			}
			for (const FRichCurveEditInfo& Entry : Curves)
			{
				if (Entry.CurveName.ToString().Equals(CurveName, ESearchCase::IgnoreCase))
				{
					OutSelected.Add(Entry);
					return true;
				}
			}
			OutError = FString::Printf(TEXT("Unknown curve_name '%s'. Valid names for this asset: %s."),
				*CurveName, *FString::Join(ValidNames, TEXT(", ")));
			return false;
		}

		int32 Index = 0;
		if (bHasCurveIndex)
		{
			double IndexNumber = 0.0;
			Arguments->TryGetNumberField(TEXT("curve_index"), IndexNumber);
			Index = static_cast<int32>(IndexNumber);
		}
		if (Index < 0 || Index >= Curves.Num())
		{
			OutError = FString::Printf(TEXT("curve_index %d out of range. Valid indices for this asset: 0..%d."),
				Index, Curves.Num() - 1);
			return false;
		}

		OutSelected.Add(Curves[Index]);
		return true;
	}
}
