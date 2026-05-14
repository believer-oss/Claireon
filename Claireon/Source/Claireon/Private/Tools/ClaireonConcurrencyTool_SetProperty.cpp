// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonConcurrencyTool_SetProperty.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundConcurrency.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	// I4: discriminator-prefixed file-local helper (avoids unity collisions across cohort .cpp files).
	FString ConcurrencySetProperty_JsonValueToString(const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid()) return FString();
		FString S;
		if (V->TryGetString(S)) return S;
		double N;
		bool B;
		if (V->TryGetNumber(N))
		{
			if (FMath::IsFinite(N) && FMath::Floor(N) == N && FMath::Abs(N) < 1e15)
			{
				return FString::Printf(TEXT("%lld"), (int64)N);
			}
			return FString::Printf(TEXT("%g"), N);
		}
		if (V->TryGetBool(B)) return B ? TEXT("true") : TEXT("false");
		return FString();
	}
}

FString FClaireonConcurrencyTool_SetProperty::GetCategory() const { return TEXT("concurrency"); }
FString FClaireonConcurrencyTool_SetProperty::GetOperation() const { return TEXT("set_property"); }

FString FClaireonConcurrencyTool_SetProperty::GetDescription() const
{
	return TEXT("Set a property on a USoundConcurrency asset by reflection. Concurrency.ResolutionRule "
				"accepts either an enum name (e.g. 'StopOldest') or an integer index. Wraps in "
				"FScopedTransaction; saves on success.");
}

TSharedPtr<FJsonObject> FClaireonConcurrencyTool_SetProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the USoundConcurrency asset"), true);
	S.AddString(TEXT("property_path"), TEXT("Dot-path of the property to set (e.g. Concurrency.MaxCount). 'Concurrency.' prefix is added automatically if missing."), true);
	S.AddString(TEXT("value"), TEXT("Value to write. ResolutionRule accepts enum name OR integer."), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonConcurrencyTool_SetProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString PropertyPath;
	if (!Arguments->TryGetStringField(TEXT("property_path"), PropertyPath) || PropertyPath.IsEmpty())
	{
		FString FieldName;
		if (!Arguments->TryGetStringField(TEXT("field_name"), FieldName) || FieldName.IsEmpty())
		{
			return MakeErrorResult(TEXT("Missing required parameter: property_path"));
		}
		PropertyPath = FString::Printf(TEXT("Concurrency.%s"), *FieldName);
	}
	else if (!PropertyPath.StartsWith(TEXT("Concurrency.")))
	{
		PropertyPath = FString::Printf(TEXT("Concurrency.%s"), *PropertyPath);
	}

	const TSharedPtr<FJsonValue> ValueJson = Arguments->TryGetField(TEXT("value"));
	if (!ValueJson.IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}
	const FString ValueStr = ConcurrencySetProperty_JsonValueToString(ValueJson);

	FString Error;
	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	UObject* Loaded = ClaireonAudioHelpers::LoadAudioAsset(AssetPath, Kind, Error);
	if (!Loaded)
	{
		return MakeErrorResult(Error);
	}
	USoundConcurrency* Conc = Cast<USoundConcurrency>(Loaded);
	if (!Conc || Kind != EClaireonAudioAssetKind::Concurrency)
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset is not a SoundConcurrency: %s"), *AssetPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Concurrency Property")));
	Conc->Modify();

	if (!ClaireonPropertyUtils::WritePropertyByPath(Conc, PropertyPath, ValueStr, Error))
	{
		return MakeErrorResult(FString::Printf(TEXT("Property not found or unwritable: %s (%s)"), *PropertyPath, *Error));
	}

	Conc->MarkPackageDirty();
	if (!ClaireonAssetUtils::SaveAsset(Conc, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Conc->GetPathName());
	Data->SetStringField(TEXT("property_path"), PropertyPath);
	Data->SetStringField(TEXT("value"), ValueStr);
	return MakeSuccessResult(Data, FString::Printf(TEXT("Set %s = %s"), *PropertyPath, *ValueStr));
}
