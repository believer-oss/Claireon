// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_SequenceActorPlace.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "GameFramework/Actor.h"
#include "LevelSequence.h"
#include "LevelSequenceActor.h"
#include "Misc/Paths.h"
#include "MovieSceneSequencePlaybackSettings.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_SequenceActorPlace::GetCategory() const { return TEXT("level"); }
FString ClaireonTool_SequenceActorPlace::GetOperation() const { return TEXT("sequence_actor_place"); }

FString ClaireonTool_SequenceActorPlace::GetDescription() const
{
	return TEXT("Place an ALevelSequenceActor (or AReplicatedLevelSequenceActor) into the "
				"currently-open map and bind it to a Level Sequence asset. Applies "
				"PlaybackSettings via reflection, marks the map package dirty, and optionally "
				"saves the map (opt-in via save_map).");
}

FString ClaireonTool_SequenceActorPlace::GetFullDescription() const
{
	return TEXT(
		"Place an ALevelSequenceActor into the current level.\n"
		"\n"
		"Spawns a Level Sequence actor at origin, binds the given ULevelSequence asset via\n"
		"SetSequence, sets the actor label, applies the provided playback_settings JSON to\n"
		"the actor's PlaybackSettings struct via FProperty::ImportText_Direct (unknown\n"
		"fields warn-not-fail), and marks the map package dirty. When save_map=true, the\n"
		"map is saved after placement.\n"
		"\n"
		"Required: map_path, sequence_asset, actor_label.\n"
		"Optional: replicated (default false), playback_settings (object), save_map\n"
		"(default false).\n"
		"\n"
		"Example input:\n"
		"  {\n"
		"    \"map_path\": \"/Game/Maps/Cinematic/Intro.Intro\",\n"
		"    \"sequence_asset\": \"/Game/Cinematics/LS_Intro.LS_Intro\",\n"
		"    \"actor_label\": \"LSA_Intro\",\n"
		"    \"replicated\": false,\n"
		"    \"playback_settings\": { \"bAutoPlay\": true, \"PlayRate\": 1.0 },\n"
		"    \"save_map\": false\n"
		"  }\n"
		"\n"
		"Example output shape (success):\n"
		"  {\n"
		"    \"ok\": true,\n"
		"    \"actor_path\": \".../PersistentLevel.LSA_Intro\",\n"
		"    \"actor_label\": \"LSA_Intro\",\n"
		"    \"map_saved\": false\n"
		"  }\n"
		"\n"
		"PIE guard: this tool mutates the editor world and fails if PIE is running.");
}

TSharedPtr<FJsonObject> ClaireonTool_SequenceActorPlace::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> MapPathProp = MakeShared<FJsonObject>();
	MapPathProp->SetStringField(TEXT("type"), TEXT("string"));
	MapPathProp->SetStringField(TEXT("description"),
		TEXT("Asset path of the map to place the sequence actor in. If the map is not "
			 "currently open, it will be loaded first via UEditorLoadingAndSavingUtils::LoadMap."));
	Properties->SetObjectField(TEXT("map_path"), MapPathProp);

	TSharedPtr<FJsonObject> SequenceAssetProp = MakeShared<FJsonObject>();
	SequenceAssetProp->SetStringField(TEXT("type"), TEXT("string"));
	SequenceAssetProp->SetStringField(TEXT("description"),
		TEXT("Asset path of the ULevelSequence to bind (e.g. /Game/Cinematics/LS_Intro.LS_Intro)."));
	Properties->SetObjectField(TEXT("sequence_asset"), SequenceAssetProp);

	TSharedPtr<FJsonObject> ActorLabelProp = MakeShared<FJsonObject>();
	ActorLabelProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorLabelProp->SetStringField(TEXT("description"),
		TEXT("Actor label to apply via AActor::SetActorLabel."));
	Properties->SetObjectField(TEXT("actor_label"), ActorLabelProp);

	TSharedPtr<FJsonObject> ReplicatedProp = MakeShared<FJsonObject>();
	ReplicatedProp->SetStringField(TEXT("type"), TEXT("boolean"));
	ReplicatedProp->SetStringField(TEXT("description"),
		TEXT("If true, spawn AReplicatedLevelSequenceActor; otherwise ALevelSequenceActor. "
			 "Default: false."));
	Properties->SetObjectField(TEXT("replicated"), ReplicatedProp);

	TSharedPtr<FJsonObject> PlaybackSettingsProp = MakeShared<FJsonObject>();
	PlaybackSettingsProp->SetStringField(TEXT("type"), TEXT("object"));
	PlaybackSettingsProp->SetStringField(TEXT("description"),
		TEXT("Free-form JSON object whose fields are applied to the actor's PlaybackSettings "
			 "struct via FProperty::ImportText_Direct. Unknown fields warn-not-fail."));
	Properties->SetObjectField(TEXT("playback_settings"), PlaybackSettingsProp);

	TSharedPtr<FJsonObject> SaveMapProp = MakeShared<FJsonObject>();
	SaveMapProp->SetStringField(TEXT("type"), TEXT("boolean"));
	SaveMapProp->SetStringField(TEXT("description"),
		TEXT("If true, call UEditorLoadingAndSavingUtils::SaveMap after placement. Default: false. "
			 "Without this, the map package is marked dirty (parity with place_actor)."));
	Properties->SetObjectField(TEXT("save_map"), SaveMapProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("map_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("sequence_asset")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("actor_label")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

namespace
{
	// Ensure path has a .ObjectName suffix so UEditorLoadingAndSavingUtils::LoadMap
	// and resolver canonicalization can consume it.
	FString AppendObjectNameIfMissing(const FString& InPath)
	{
		if (InPath.Contains(TEXT(".")))
		{
			return InPath;
		}
		FString AssetName = FPaths::GetBaseFilename(InPath);
		return InPath + TEXT(".") + AssetName;
	}

	// Strip .ObjectName suffix to get the package path that UWorld::GetOutermost() returns
	// (which is of the form /Game/Maps/MyLevel, not /Game/Maps/MyLevel.MyLevel).
	FString StripObjectNameSuffix(const FString& InPath)
	{
		int32 DotIdx = INDEX_NONE;
		if (InPath.FindChar(TEXT('.'), DotIdx))
		{
			return InPath.Left(DotIdx);
		}
		return InPath;
	}
} // namespace

FToolResult ClaireonTool_SequenceActorPlace::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments object"));
	}

	// --- Parse inputs ------------------------------------------------------------
	FString MapPath;
	if (!Arguments->TryGetStringField(TEXT("map_path"), MapPath) || MapPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: map_path"));
	}

	FString SequenceAssetPath;
	if (!Arguments->TryGetStringField(TEXT("sequence_asset"), SequenceAssetPath) || SequenceAssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: sequence_asset"));
	}

	FString ActorLabel;
	if (!Arguments->TryGetStringField(TEXT("actor_label"), ActorLabel) || ActorLabel.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: actor_label"));
	}

	bool bReplicated = false;
	Arguments->TryGetBoolField(TEXT("replicated"), bReplicated);

	bool bSaveMap = false;
	Arguments->TryGetBoolField(TEXT("save_map"), bSaveMap);

	const TSharedPtr<FJsonObject>* PlaybackSettingsObjPtr = nullptr;
	Arguments->TryGetObjectField(TEXT("playback_settings"), PlaybackSettingsObjPtr);

	// --- Resolve paths ------------------------------------------------------------
	{
		auto ResolveResult = ClaireonPathResolver::Resolve(SequenceAssetPath);
		if (ResolveResult.bSuccess)
		{
			SequenceAssetPath = ResolveResult.ResolvedPath.Path;
		}
	}
	{
		auto ResolveResult = ClaireonPathResolver::Resolve(MapPath);
		if (ResolveResult.bSuccess)
		{
			MapPath = ResolveResult.ResolvedPath.Path;
		}
	}
	MapPath = AppendObjectNameIfMissing(MapPath);

	// --- Load Level Sequence asset -----------------------------------------------
	ULevelSequence* Sequence = Cast<ULevelSequence>(
		StaticLoadObject(ULevelSequence::StaticClass(), nullptr, *SequenceAssetPath));
	if (!Sequence)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Could not load Level Sequence asset: %s"), *SequenceAssetPath));
	}

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	// --- Ensure target map is the current editor world ----------------------------
	UWorld* CurrentWorld = GEditor->GetEditorWorldContext().World();
	const FString MapPackagePath = StripObjectNameSuffix(MapPath);
	const bool bMapAlreadyOpen = CurrentWorld &&
		CurrentWorld->GetOutermost() &&
		CurrentWorld->GetOutermost()->GetName() == MapPackagePath;

	if (!bMapAlreadyOpen)
	{
		UWorld* LoadedWorld = UEditorLoadingAndSavingUtils::LoadMap(MapPath);
		if (!LoadedWorld)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Failed to load map: %s"), *MapPath));
		}
		CurrentWorld = GEditor->GetEditorWorldContext().World();
	}

	if (!CurrentWorld)
	{
		return MakeErrorResult(TEXT("No editor world available after map load"));
	}

	// --- Resolve spawn class ------------------------------------------------------
	UClass* ActorClass = bReplicated
		? AReplicatedLevelSequenceActor::StaticClass()
		: ALevelSequenceActor::StaticClass();

	// --- Spawn + configure under a scoped transaction ----------------------------
	UEditorActorSubsystem* EditorActorSubsystem = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
	if (!EditorActorSubsystem)
	{
		return MakeErrorResult(TEXT("UEditorActorSubsystem not available"));
	}

	TArray<FString> WarningsLocal;
	AActor* SpawnedActor = nullptr;
	{
		FScopedTransaction Transaction(FText::FromString(TEXT("Claireon: sequence_actor_place")));

		SpawnedActor = EditorActorSubsystem->SpawnActorFromClass(
			ActorClass, FVector::ZeroVector, FRotator::ZeroRotator, /*bTransient=*/false);
		if (!SpawnedActor)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Failed to spawn actor of class %s"), *ActorClass->GetName()));
		}

		SpawnedActor->SetActorLabel(ActorLabel, /*bMarkDirty=*/true);

		ALevelSequenceActor* SequenceActor = Cast<ALevelSequenceActor>(SpawnedActor);
		if (!SequenceActor)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Spawned actor is not a LevelSequenceActor: %s"), *SpawnedActor->GetClass()->GetName()));
		}

		SequenceActor->SetSequence(Sequence);

		// --- Apply playback_settings via reflection -----------------------------
		if (PlaybackSettingsObjPtr && PlaybackSettingsObjPtr->IsValid())
		{
			UScriptStruct* SettingsStruct = FMovieSceneSequencePlaybackSettings::StaticStruct();
			FProperty* PlaybackSettingsProp =
				FindFProperty<FProperty>(ALevelSequenceActor::StaticClass(), TEXT("PlaybackSettings"));
			if (!PlaybackSettingsProp)
			{
				WarningsLocal.Add(TEXT("ALevelSequenceActor::PlaybackSettings property not found via reflection"));
			}
			else
			{
				void* PlaybackSettingsPtr = PlaybackSettingsProp->ContainerPtrToValuePtr<void>(SequenceActor);
				for (const auto& Pair : (*PlaybackSettingsObjPtr)->Values)
				{
					FProperty* FieldProp = FindFProperty<FProperty>(SettingsStruct, *Pair.Key);
					if (!FieldProp)
					{
						const FString Msg = FString::Printf(
							TEXT("[sequence_actor_place] Unknown PlaybackSettings field '%s' (ignored)"),
							*Pair.Key);
						UE_LOG(LogClaireon, Warning, TEXT("%s"), *Msg);
						WarningsLocal.Add(Msg);
						continue;
					}

					// Stringify JSON value for ImportText_Direct.
					FString ValueStr;
					if (!Pair.Value->TryGetString(ValueStr))
					{
						double NumVal = 0.0;
						bool BoolVal = false;
						if (Pair.Value->TryGetNumber(NumVal))
						{
							// Use integer form when the value is integral to avoid FPU noise for ints.
							if (FMath::IsNearlyEqual(NumVal, FMath::RoundToDouble(NumVal)))
							{
								ValueStr = FString::FromInt(static_cast<int64>(NumVal));
							}
							else
							{
								ValueStr = FString::SanitizeFloat(NumVal);
							}
						}
						else if (Pair.Value->TryGetBool(BoolVal))
						{
							ValueStr = BoolVal ? TEXT("True") : TEXT("False");
						}
						else
						{
							// Fallback to stringified JSON for nested objects (e.g. LoopCount).
							TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> JsonWriter =
								TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&ValueStr);
							FJsonSerializer::Serialize(Pair.Value.ToSharedRef(), FString(), JsonWriter);
						}
					}

					void* FieldValuePtr = FieldProp->ContainerPtrToValuePtr<void>(PlaybackSettingsPtr);
					const TCHAR* ImportResult = FieldProp->ImportText_Direct(
						*ValueStr, FieldValuePtr, SequenceActor, PPF_None);
					if (!ImportResult)
					{
						const FString Msg = FString::Printf(
							TEXT("[sequence_actor_place] Failed to parse PlaybackSettings.%s='%s'"),
							*Pair.Key, *ValueStr);
						UE_LOG(LogClaireon, Warning, TEXT("%s"), *Msg);
						WarningsLocal.Add(Msg);
					}
				}
			}
		}

		// --- MarkPackageDirty (parity with place_actor) ------------------
		if (UPackage* OuterPackage = CurrentWorld->GetOutermost())
		{
			OuterPackage->MarkPackageDirty();
		}
	}

	// --- Optional SaveMap ---------------------------------------------------------
	bool bMapSaved = false;
	if (bSaveMap)
	{
		bMapSaved = UEditorLoadingAndSavingUtils::SaveMap(CurrentWorld, MapPackagePath);
		if (!bMapSaved)
		{
			WarningsLocal.Add(FString::Printf(
				TEXT("SaveMap returned false for %s"), *MapPackagePath));
		}
	}

	// --- Build structured output --------------------------------------------------
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("ok"), true);
	Data->SetStringField(TEXT("actor_path"), SpawnedActor->GetPathName());
	Data->SetStringField(TEXT("actor_label"), SpawnedActor->GetActorLabel());
	Data->SetStringField(TEXT("actor_class"), SpawnedActor->GetClass()->GetName());
	Data->SetBoolField(TEXT("map_saved"), bMapSaved);

	FString Summary = FString::Printf(
		TEXT("Placed %s '%s' in %s%s"),
		*SpawnedActor->GetClass()->GetName(),
		*SpawnedActor->GetActorLabel(),
		*MapPackagePath,
		bMapSaved ? TEXT(" (map saved)") : TEXT(""));

	FToolResult Result = MakeSuccessResult(Data, Summary);
	Result.Warnings = MoveTemp(WarningsLocal);
	return Result;
}
