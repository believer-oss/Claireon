// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AudioInspect.h"
#include "Tools/ClaireonAudioHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

#include "Sound/SoundCue.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"
#include "Sound/SoundAttenuation.h"
#include "Sound/SoundConcurrency.h"

#if __has_include("MetasoundSource.h")
#include "MetasoundSource.h"
#endif

FString FClaireonTool_AudioInspect::GetCategory() const { return TEXT("audio"); }
FString FClaireonTool_AudioInspect::GetOperation() const { return TEXT("inspect"); }

FString FClaireonTool_AudioInspect::GetDescription() const
{
	return TEXT("Read the structure of an audio asset (SoundCue, MetaSoundSource, SoundClass, "
				"SoundMix, SoundAttenuation, or SoundConcurrency). Stateless -- no session required. "
				"Emits a top-level `kind` field and a kind-specific body. ResolutionRule on "
				"concurrency output is always an enum name string, never an integer.");
}

TSharedPtr<FJsonObject> FClaireonTool_AudioInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Unreal asset path to the audio asset."));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"), TEXT("Level of detail: 'summary' or 'full' (default 'full')."));
	{
		TArray<TSharedPtr<FJsonValue>> EnumVals;
		EnumVals.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		EnumVals.Add(MakeShared<FJsonValueString>(TEXT("full")));
		DetailProp->SetArrayField(TEXT("enum"), EnumVals);
	}
	Properties->SetObjectField(TEXT("detail_level"), DetailProp);

	TSharedPtr<FJsonObject> FocusProp = MakeShared<FJsonObject>();
	FocusProp->SetStringField(TEXT("type"), TEXT("string"));
	FocusProp->SetStringField(TEXT("description"),
		TEXT("Asset-kind-specific focus selector (SoundCue: node index; MetaSound: input/output name; SoundClass: child class name)."));
	Properties->SetObjectField(TEXT("focus"), FocusProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult FClaireonTool_AudioInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path"));
	}

	FString DetailLevelStr = TEXT("full");
	Arguments->TryGetStringField(TEXT("detail_level"), DetailLevelStr);
	const EClaireonAudioDetailLevel DetailLevel = (DetailLevelStr.Equals(TEXT("summary"), ESearchCase::IgnoreCase))
		? EClaireonAudioDetailLevel::Summary
		: EClaireonAudioDetailLevel::Full;

	FString FocusHint;
	Arguments->TryGetStringField(TEXT("focus"), FocusHint);

	EClaireonAudioAssetKind Kind = EClaireonAudioAssetKind::Unknown;
	FString LoadError;
	UObject* Obj = ClaireonAudioHelpers::LoadAudioAsset(AssetPath, Kind, LoadError);
	if (!Obj)
	{
		return MakeErrorResult(LoadError);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("kind"), AudioAssetKindToString(Kind));

	FString Summary;

	switch (Kind)
	{
	case EClaireonAudioAssetKind::SoundCue:
	{
		const USoundCue* SC = Cast<USoundCue>(Obj);
		ClaireonAudioHelpers::FormatSoundCueGraph(SC, Data.ToSharedRef(), DetailLevel, FocusHint);
		int32 NodeCount = 0;
#if WITH_EDITORONLY_DATA
		if (SC) NodeCount = SC->AllNodes.Num();
#endif
		Summary = FString::Printf(TEXT("SoundCue %s: %d nodes"), *SC->GetName(), NodeCount);
		break;
	}
#if __has_include("MetasoundSource.h")
	case EClaireonAudioAssetKind::MetaSoundSource:
	{
		const UMetaSoundSource* MS = Cast<UMetaSoundSource>(Obj);
		ClaireonAudioHelpers::FormatMetaSoundDocument(MS, Data.ToSharedRef(), DetailLevel, FocusHint);
		Summary = FString::Printf(TEXT("MetaSoundSource %s"), *MS->GetName());
		break;
	}
#endif
	case EClaireonAudioAssetKind::SoundClass:
	{
		const USoundClass* Cls = Cast<USoundClass>(Obj);
		Data->SetStringField(TEXT("asset_path"), Cls->GetPathName());

		TSharedPtr<FJsonObject> PropsJson = MakeShared<FJsonObject>();
		// IterateSoundClassPropertiesStruct -- const_cast is safe because we only read through the callback.
		ClaireonAudioHelpers::IterateSoundClassPropertiesStruct(
			const_cast<FSoundClassProperties&>(Cls->Properties),
			[&PropsJson](FProperty* Prop, void* ValuePtr)
		{
			if (!Prop || !ValuePtr) return;
			FString Exported;
			Prop->ExportText_Direct(Exported, ValuePtr, ValuePtr, nullptr, PPF_None);
			PropsJson->SetStringField(Prop->GetName(), Exported);
		});
		Data->SetObjectField(TEXT("properties"), PropsJson);

		TArray<TSharedPtr<FJsonValue>> ChildPaths;
		for (const TObjectPtr<USoundClass>& Child : Cls->ChildClasses)
		{
			if (Child)
			{
				ChildPaths.Add(MakeShared<FJsonValueString>(Child->GetPathName()));
			}
		}
		Data->SetArrayField(TEXT("child_class_paths"), ChildPaths);
		Summary = FString::Printf(TEXT("SoundClass %s: %d children"), *Cls->GetName(), Cls->ChildClasses.Num());
		break;
	}
	case EClaireonAudioAssetKind::SoundMix:
	{
		const USoundMix* Mix = Cast<USoundMix>(Obj);
		Data->SetStringField(TEXT("asset_path"), Mix->GetPathName());
		Data->SetNumberField(TEXT("initial_delay"), Mix->InitialDelay);
		Data->SetNumberField(TEXT("fade_in_time"), Mix->FadeInTime);
		Data->SetNumberField(TEXT("duration"), Mix->Duration);
		Data->SetNumberField(TEXT("fade_out_time"), Mix->FadeOutTime);

		TArray<TSharedPtr<FJsonValue>> Effects;
		for (const FSoundClassAdjuster& Adj : Mix->SoundClassEffects)
		{
			TSharedPtr<FJsonObject> E = MakeShared<FJsonObject>();
			E->SetStringField(TEXT("sound_class"), Adj.SoundClassObject ? Adj.SoundClassObject->GetPathName() : FString());
			E->SetNumberField(TEXT("volume_adjuster"), Adj.VolumeAdjuster);
			E->SetNumberField(TEXT("pitch_adjuster"), Adj.PitchAdjuster);
			E->SetNumberField(TEXT("lowpass_frequency"), Adj.LowPassFilterFrequency);
			E->SetNumberField(TEXT("voice_center_channel_volume_adjuster"), Adj.VoiceCenterChannelVolumeAdjuster);
			E->SetBoolField(TEXT("apply_to_children"), Adj.bApplyToChildren != 0);
			Effects.Add(MakeShared<FJsonValueObject>(E));
		}
		Data->SetArrayField(TEXT("sound_class_effects"), Effects);
		Summary = FString::Printf(TEXT("SoundMix %s: %d effects"), *Mix->GetName(), Mix->SoundClassEffects.Num());
		break;
	}
	case EClaireonAudioAssetKind::Attenuation:
	{
		const USoundAttenuation* Att = Cast<USoundAttenuation>(Obj);
		ClaireonAudioHelpers::FormatAttenuationSettings(Att, Data.ToSharedRef());
		Summary = FString::Printf(TEXT("Attenuation %s"), *Att->GetName());
		break;
	}
	case EClaireonAudioAssetKind::Concurrency:
	{
		const USoundConcurrency* Con = Cast<USoundConcurrency>(Obj);
		ClaireonAudioHelpers::FormatConcurrencySettings(Con, Data.ToSharedRef());
		Summary = FString::Printf(TEXT("Concurrency %s"), *Con->GetName());
		break;
	}
	default:
		return MakeErrorResult(FString::Printf(
			TEXT("Unsupported audio asset kind for inspect (class=%s)"), *Obj->GetClass()->GetName()));
	}

	return MakeSuccessResult(Data, Summary);
}
