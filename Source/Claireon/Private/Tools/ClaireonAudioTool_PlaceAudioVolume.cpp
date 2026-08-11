// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAudioTool_PlaceAudioVolume.h"
#include "Tools/ClaireonAudioApplyHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Sound/AudioVolume.h"

using FToolResult = IClaireonTool::FToolResult;

FString FClaireonAudioTool_PlaceAudioVolume::GetCategory() const { return TEXT("audio"); }
FString FClaireonAudioTool_PlaceAudioVolume::GetOperation() const { return TEXT("place_audio_volume"); }

FString FClaireonAudioTool_PlaceAudioVolume::GetDescription() const
{
	return TEXT("Spawn an AAudioVolume actor in the current editor world at the supplied transform. "
	            "Optional 'label' renames the actor; optional 'properties' is a reflection-write blob "
	            "for fields on the spawned actor (warnings, not errors, for unsettable fields). "
	            "Stateless / non-session: edits the editor world directly inside an FScopedTransaction "
	            "(editor undo works), so no open session is required.");
}

TSharedPtr<FJsonObject> FClaireonAudioTool_PlaceAudioVolume::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	ClaireonAudioSchema::AddString(Properties, TEXT("label"), TEXT("Actor label to give the placed actor."));
	ClaireonAudioSchema::AddObject(Properties, TEXT("transform"), TEXT("Placement transform as {location, rotation, scale}."));
	ClaireonAudioSchema::AddObject(Properties, TEXT("properties"), TEXT("Property name/value map applied to the placed actor."));

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("transform")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult FClaireonAudioTool_PlaceAudioVolume::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!IsValid(GEditor) || !IsValid(GEditor->GetEditorWorldContext().World()))
	{
		return MakeErrorResult(TEXT("place_audio_volume requires an active editor world"));
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();

	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	FTransform Xform;
	FString XformError;
	if (!ClaireonAudioApplyHelpers::ParseTransformField(Arguments, Xform, XformError)) return MakeErrorResult(XformError);

	FString Label;
	Arguments->TryGetStringField(TEXT("label"), Label);

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Place Audio Volume")));

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AAudioVolume* Actor = World->SpawnActor<AAudioVolume>(AAudioVolume::StaticClass(),
		Xform.GetLocation(), Xform.GetRotation().Rotator(), Params);
	if (!IsValid(Actor)) return MakeErrorResult(TEXT("Failed to spawn AAudioVolume"));

	Actor->SetActorScale3D(Xform.GetScale3D());
	if (!Label.IsEmpty()) Actor->SetActorLabel(Label, /*bMarkDirty=*/true);

	TArray<FString> Warnings;
	const TSharedPtr<FJsonObject>* PropsObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
	{
		ClaireonAudioApplyHelpers::WriteReflectedProperties(Actor, *PropsObj, Warnings);
	}
	Actor->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
	Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
	FToolResult Result = MakeSuccessResult(Data, FString::Printf(TEXT("Placed AudioVolume %s"), *Actor->GetActorLabel()));
	Result.Warnings = MoveTemp(Warnings);
	return Result;
}
