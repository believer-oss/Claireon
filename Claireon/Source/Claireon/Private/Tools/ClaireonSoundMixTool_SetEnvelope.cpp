// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundMixTool_SetEnvelope.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundMix.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundMixTool_SetEnvelope::GetCategory() const { return TEXT("soundmix"); }
FString FClaireonSoundMixTool_SetEnvelope::GetOperation() const { return TEXT("set_envelope"); }

FString FClaireonSoundMixTool_SetEnvelope::GetDescription() const
{
	return TEXT("Set the envelope (initial_delay/fade_in_time/duration/fade_out_time) on a USoundMix. "
				"Each field is optional; only supplied fields are written.");
}

TSharedPtr<FJsonObject> FClaireonSoundMixTool_SetEnvelope::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the USoundMix asset"), true);
	S.AddNumber(TEXT("initial_delay"), TEXT("Optional initial delay in seconds"));
	S.AddNumber(TEXT("fade_in_time"), TEXT("Optional fade-in time in seconds"));
	S.AddNumber(TEXT("duration"), TEXT("Optional duration in seconds"));
	S.AddNumber(TEXT("fade_out_time"), TEXT("Optional fade-out time in seconds"));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundMixTool_SetEnvelope::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set SoundMix Envelope")));
	Mix->Modify();

	double V;
	bool bAnyWritten = false;
	if (Arguments->TryGetNumberField(TEXT("initial_delay"), V)) { Mix->InitialDelay = static_cast<float>(V); bAnyWritten = true; }
	if (Arguments->TryGetNumberField(TEXT("fade_in_time"), V))  { Mix->FadeInTime = static_cast<float>(V);  bAnyWritten = true; }
	if (Arguments->TryGetNumberField(TEXT("duration"), V))      { Mix->Duration = static_cast<float>(V);    bAnyWritten = true; }
	if (Arguments->TryGetNumberField(TEXT("fade_out_time"), V)) { Mix->FadeOutTime = static_cast<float>(V); bAnyWritten = true; }

	if (bAnyWritten)
	{
		Mix->MarkPackageDirty();
		if (!ClaireonAssetUtils::SaveAsset(Mix, Error))
		{
			return MakeErrorResult(Error);
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Mix->GetPathName());
	Data->SetNumberField(TEXT("initial_delay"), Mix->InitialDelay);
	Data->SetNumberField(TEXT("fade_in_time"), Mix->FadeInTime);
	Data->SetNumberField(TEXT("duration"), Mix->Duration);
	Data->SetNumberField(TEXT("fade_out_time"), Mix->FadeOutTime);
	return MakeSuccessResult(Data, TEXT("SoundMix envelope updated"));
}
