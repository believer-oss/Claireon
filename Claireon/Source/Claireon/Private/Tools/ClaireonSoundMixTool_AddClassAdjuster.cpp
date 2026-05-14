// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundMixTool_AddClassAdjuster.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder
#include "ClaireonPathResolver.h"

#include "Sound/SoundMix.h"
#include "Sound/SoundClass.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundMixTool_AddClassAdjuster::GetCategory() const { return TEXT("soundmix"); }
FString FClaireonSoundMixTool_AddClassAdjuster::GetOperation() const { return TEXT("add_class_adjuster"); }

FString FClaireonSoundMixTool_AddClassAdjuster::GetDescription() const
{
	return TEXT("Append a SoundClass adjuster (FSoundClassAdjuster) to USoundMix::SoundClassEffects. "
				"Rejects duplicates if the same SoundClass is already adjusted.");
}

TSharedPtr<FJsonObject> FClaireonSoundMixTool_AddClassAdjuster::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the USoundMix asset"), true);
	S.AddString(TEXT("sound_class_path"), TEXT("Path of the USoundClass to adjust"), true);
	S.AddNumber(TEXT("volume_adjuster"), TEXT("Optional: VolumeAdjuster (default 1.0)"));
	S.AddNumber(TEXT("pitch_adjuster"), TEXT("Optional: PitchAdjuster (default 1.0)"));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundMixTool_AddClassAdjuster::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	FString TargetPath;
	if (!Arguments->TryGetStringField(TEXT("sound_class_path"), TargetPath) || TargetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: sound_class_path"));
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

	const ClaireonPathResolver::FResolveResult Resolved = ClaireonPathResolver::Resolve(TargetPath);
	USoundClass* Target = Resolved.bSuccess ? LoadObject<USoundClass>(nullptr, *Resolved.ResolvedPath.Path) : nullptr;
	if (!Target)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not load SoundClass at %s"), *TargetPath));
	}

	// Reject duplicate (matches sub-doc note "rejects duplicates").
	for (const FSoundClassAdjuster& Existing : Mix->SoundClassEffects)
	{
		if (Existing.SoundClassObject && Existing.SoundClassObject->GetPathName() == Target->GetPathName())
		{
			return MakeErrorResult(FString::Printf(TEXT("SoundClass already adjusted in this mix: %s"), *Target->GetPathName()));
		}
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add SoundMix Class Adjuster")));
	Mix->Modify();
	FSoundClassAdjuster Adj;
	Adj.SoundClassObject = Target;
	double V;
	if (Arguments->TryGetNumberField(TEXT("volume_adjuster"), V)) Adj.VolumeAdjuster = static_cast<float>(V);
	if (Arguments->TryGetNumberField(TEXT("pitch_adjuster"), V)) Adj.PitchAdjuster = static_cast<float>(V);
	const int32 NewIndex = Mix->SoundClassEffects.Add(Adj);
	Mix->MarkPackageDirty();
	if (!ClaireonAssetUtils::SaveAsset(Mix, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Mix->GetPathName());
	Data->SetNumberField(TEXT("adjuster_index"), NewIndex);
	Data->SetNumberField(TEXT("adjuster_count"), Mix->SoundClassEffects.Num());
	return MakeSuccessResult(Data, FString::Printf(TEXT("Added class adjuster for %s at index %d"),
		*Target->GetPathName(), NewIndex));
}
