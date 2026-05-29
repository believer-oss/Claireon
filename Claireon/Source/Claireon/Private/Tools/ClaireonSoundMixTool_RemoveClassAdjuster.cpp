// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonSoundMixTool_RemoveClassAdjuster.h"
#include "Tools/ClaireonAudioHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonAnimEditToolBase.h" // FToolSchemaBuilder

#include "Sound/SoundMix.h"
#include "Sound/SoundClass.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"

FString FClaireonSoundMixTool_RemoveClassAdjuster::GetCategory() const { return TEXT("soundmix"); }
FString FClaireonSoundMixTool_RemoveClassAdjuster::GetOperation() const { return TEXT("remove_class_adjuster"); }

FString FClaireonSoundMixTool_RemoveClassAdjuster::GetDescription() const
{
	return TEXT("Remove a class adjuster from USoundMix::SoundClassEffects by adjuster_index or by "
				"sound_class_path. Non-session, immediate operation; saves the mix asset on success. "
				"Returns removed_count; 0 if the adjuster was not found.");
}

TSharedPtr<FJsonObject> FClaireonSoundMixTool_RemoveClassAdjuster::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the USoundMix asset"), true);
	S.AddInteger(TEXT("adjuster_index"), TEXT("Index of the adjuster to remove (use either this OR sound_class_path)"));
	S.AddString(TEXT("sound_class_path"), TEXT("Path of the USoundClass whose adjuster to remove (use either this OR adjuster_index)"));
	return S.Build();
}

IClaireonTool::FToolResult FClaireonSoundMixTool_RemoveClassAdjuster::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	double IndexD;
	const bool bHasIndex = Arguments->TryGetNumberField(TEXT("adjuster_index"), IndexD);
	FString TargetPath;
	const bool bHasTarget = Arguments->TryGetStringField(TEXT("sound_class_path"), TargetPath) && !TargetPath.IsEmpty();
	if (!bHasIndex && !bHasTarget)
	{
		return MakeErrorResult(TEXT("Provide adjuster_index or sound_class_path"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove SoundMix Class Adjuster")));
	Mix->Modify();

	int32 RemovedCount = 0;
	int32 RemovedIndex = INDEX_NONE;
	if (bHasIndex)
	{
		const int32 Idx = static_cast<int32>(IndexD);
		if (!Mix->SoundClassEffects.IsValidIndex(Idx))
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(TEXT("Adjuster index out of range: %d (max: %d)"),
				Idx, Mix->SoundClassEffects.Num() - 1));
		}
		Mix->SoundClassEffects.RemoveAt(Idx);
		RemovedCount = 1;
		RemovedIndex = Idx;
	}
	else
	{
		RemovedCount = Mix->SoundClassEffects.RemoveAll([&TargetPath](const FSoundClassAdjuster& A)
		{
			return A.SoundClassObject && (A.SoundClassObject->GetPathName() == TargetPath);
		});
		if (RemovedCount == 0)
		{
			Transaction.Cancel();
			return MakeErrorResult(FString::Printf(TEXT("Adjuster not found for SoundClass: %s"), *TargetPath));
		}
	}

	Mix->MarkPackageDirty();
	if (!ClaireonAssetUtils::SaveAsset(Mix, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Mix->GetPathName());
	Data->SetNumberField(TEXT("removed_count"), RemovedCount);
	if (RemovedIndex != INDEX_NONE) Data->SetNumberField(TEXT("removed_index"), RemovedIndex);
	Data->SetNumberField(TEXT("adjuster_count"), Mix->SoundClassEffects.Num());
	return MakeSuccessResult(Data, FString::Printf(TEXT("Removed %d adjuster(s)"), RemovedCount));
}
