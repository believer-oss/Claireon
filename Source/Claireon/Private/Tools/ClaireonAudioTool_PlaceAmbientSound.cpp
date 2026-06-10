// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAudioTool_PlaceAmbientSound.h"
#include "Tools/ClaireonAudioApplyHelpers.h"

#include "Components/AudioComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Sound/AmbientSound.h"
#include "Sound/SoundBase.h"

using FToolResult = IClaireonTool::FToolResult;

FString FClaireonAudioTool_PlaceAmbientSound::GetCategory() const { return TEXT("audio"); }
FString FClaireonAudioTool_PlaceAmbientSound::GetOperation() const { return TEXT("place_ambient_sound"); }

FString FClaireonAudioTool_PlaceAmbientSound::GetDescription() const
{
	return TEXT("Spawn an AAmbientSound actor in the current editor world at the supplied transform, "
	            "binding the named USoundBase asset. Optional 'auto_activate' (default true) controls the AudioComponent. "
	            "Optional 'label' renames the actor. Wrapped in FScopedTransaction so editor undo works.");
}

TSharedPtr<FJsonObject> FClaireonAudioTool_PlaceAmbientSound::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	for (const TCHAR* Field : { TEXT("sound_asset_path"), TEXT("label") })
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("string"));
		Properties->SetObjectField(Field, P);
	}
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("boolean"));
		Properties->SetObjectField(TEXT("auto_activate"), P);
	}
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("object"));
		Properties->SetObjectField(TEXT("transform"), P);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("sound_asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("transform")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult FClaireonAudioTool_PlaceAmbientSound::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor || !GEditor->GetEditorWorldContext().World())
	{
		return MakeErrorResult(TEXT("place_ambient_sound requires an active editor world"));
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();

	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	FString SoundPath;
	if (!Arguments->TryGetStringField(TEXT("sound_asset_path"), SoundPath) || SoundPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("place_ambient_sound: missing sound_asset_path"));
	}
	FString LoadError;
	USoundBase* Sound = ClaireonAudioApplyHelpers::LoadSoundBase(SoundPath, LoadError);
	if (!Sound) return MakeErrorResult(LoadError);

	FTransform Xform;
	FString XformError;
	if (!ClaireonAudioApplyHelpers::ParseTransformField(Arguments, Xform, XformError)) return MakeErrorResult(XformError);

	bool bAutoActivate = true;
	Arguments->TryGetBoolField(TEXT("auto_activate"), bAutoActivate);
	FString Label;
	Arguments->TryGetStringField(TEXT("label"), Label);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Place Ambient Sound")));

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AAmbientSound* Actor = World->SpawnActor<AAmbientSound>(AAmbientSound::StaticClass(),
		Xform.GetLocation(), Xform.GetRotation().Rotator(), Params);
	if (!Actor) return MakeErrorResult(TEXT("Failed to spawn AAmbientSound"));

	Actor->SetActorScale3D(Xform.GetScale3D());
	if (!Label.IsEmpty()) Actor->SetActorLabel(Label, /*bMarkDirty=*/true);

	if (UAudioComponent* AC = Actor->GetAudioComponent())
	{
		AC->SetSound(Sound);
		AC->bAutoActivate = bAutoActivate;
	}
	Actor->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
	Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	return MakeSuccessResult(Data, FString::Printf(TEXT("Placed AmbientSound %s"), *Actor->GetActorLabel()));
}
