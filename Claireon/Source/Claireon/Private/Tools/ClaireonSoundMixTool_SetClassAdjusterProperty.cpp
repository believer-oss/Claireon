// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundMixTool_SetClassAdjusterProperty.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundMix.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	FString SoundMixSetAdjuster_JsonValueToString(const TSharedPtr<FJsonValue>& V)
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

FString FClaireonSoundMixTool_SetClassAdjusterProperty::GetCategory() const { return TEXT("soundmix"); }
FString FClaireonSoundMixTool_SetClassAdjusterProperty::GetOperation() const { return TEXT("set_class_adjuster_property"); }

FString FClaireonSoundMixTool_SetClassAdjusterProperty::GetDescription() const
{
	return TEXT("Set a sub-property on a USoundMix's class adjuster (FSoundClassAdjuster) by index "
				"and field name (e.g. VolumeAdjuster, PitchAdjuster, LowPassFilterAdjuster, bApplyToChildren).");
}

TSharedPtr<FJsonObject> FClaireonSoundMixTool_SetClassAdjusterProperty::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the USoundMix asset"), true);
	S.AddInteger(TEXT("adjuster_index"), TEXT("Index into SoundClassEffects"), true);
	S.AddString(TEXT("property_path"), TEXT("Sub-field name on FSoundClassAdjuster (e.g. VolumeAdjuster)"), true);
	S.AddString(TEXT("value"), TEXT("Value to write."), true);
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundMixTool_SetClassAdjusterProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	double IndexD;
	if (!Arguments->TryGetNumberField(TEXT("adjuster_index"), IndexD))
	{
		// Backward-compat with bundled tool's "index" name.
		if (!Arguments->TryGetNumberField(TEXT("index"), IndexD))
		{
			return MakeErrorResult(TEXT("Missing required parameter: adjuster_index"));
		}
	}
	const int32 Index = static_cast<int32>(IndexD);

	FString PropertyPath;
	if (!Arguments->TryGetStringField(TEXT("property_path"), PropertyPath) || PropertyPath.IsEmpty())
	{
		if (!Arguments->TryGetStringField(TEXT("field_name"), PropertyPath) || PropertyPath.IsEmpty())
		{
			return MakeErrorResult(TEXT("Missing required parameter: property_path"));
		}
	}
	const TSharedPtr<FJsonValue> ValueJson = Arguments->TryGetField(TEXT("value"));
	if (!ValueJson.IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}
	const FString ValueStr = SoundMixSetAdjuster_JsonValueToString(ValueJson);

	FString Error;
	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	UObject* Loaded = ClaireonAudioHelpers::LoadAudioAsset(AssetPath, Kind, Error);
	if (!Loaded)
	{
		return MakeErrorResult(Error);
	}
	USoundMix* Mix = Cast<USoundMix>(Loaded);
	if (!Mix || Kind != EClaireonAudioAssetKind::SoundMix)
	{
		return MakeErrorResult(FString::Printf(TEXT("Asset is not a SoundMix: %s"), *AssetPath));
	}
	if (!Mix->SoundClassEffects.IsValidIndex(Index))
	{
		return MakeErrorResult(FString::Printf(TEXT("Adjuster index out of range: %d (max: %d)"),
			Index, Mix->SoundClassEffects.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set SoundMix Adjuster Property")));
	Mix->Modify();
	const FString DotPath = FString::Printf(TEXT("SoundClassEffects[%d].%s"), Index, *PropertyPath);
	if (!ClaireonPropertyUtils::WritePropertyByPath(Mix, DotPath, ValueStr, Error))
	{
		return MakeErrorResult(FString::Printf(TEXT("Property not found on FSoundClassAdjuster: %s (%s)"), *PropertyPath, *Error));
	}

	Mix->MarkPackageDirty();
	if (!ClaireonAssetUtils::SaveAsset(Mix, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Mix->GetPathName());
	Data->SetNumberField(TEXT("adjuster_index"), Index);
	Data->SetStringField(TEXT("property_path"), PropertyPath);
	Data->SetStringField(TEXT("value"), ValueStr);
	return MakeSuccessResult(Data, FString::Printf(TEXT("Set %s = %s"), *DotPath, *ValueStr));
}
