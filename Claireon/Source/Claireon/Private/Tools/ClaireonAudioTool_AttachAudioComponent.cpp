// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAudioTool_AttachAudioComponent.h"
#include "Tools/ClaireonAudioApplyHelpers.h"

#include "Components/AudioComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Sound/SoundBase.h"
#include "UObject/UObjectGlobals.h"

using FToolResult = IClaireonTool::FToolResult;

FString FClaireonAudioTool_AttachAudioComponent::GetCategory() const { return TEXT("audio"); }
FString FClaireonAudioTool_AttachAudioComponent::GetOperation() const { return TEXT("attach_audio_component"); }

FString FClaireonAudioTool_AttachAudioComponent::GetDescription() const
{
	return TEXT("Attach a new UAudioComponent to an existing actor (looked up by GetActorLabel), bound to a USoundBase. "
	            "Optional 'component_name' overrides the auto-generated 'AudioComponent_<N>'. "
	            "Optional 'auto_activate' (default true) controls AC->bAutoActivate. "
	            "Wrapped in FScopedTransaction so editor undo works.");
}

TSharedPtr<FJsonObject> FClaireonAudioTool_AttachAudioComponent::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	for (const TCHAR* Field : { TEXT("actor_name"), TEXT("sound_asset_path"), TEXT("component_name") })
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

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actor_name")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("sound_asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

FToolResult FClaireonAudioTool_AttachAudioComponent::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor || !GEditor->GetEditorWorldContext().World())
	{
		return MakeErrorResult(TEXT("attach_audio_component requires an active editor world"));
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();

	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	FString ActorName;
	if (!Arguments->TryGetStringField(TEXT("actor_name"), ActorName) || ActorName.IsEmpty())
	{
		return MakeErrorResult(TEXT("attach_audio_component: missing actor_name"));
	}
	FString SoundPath;
	if (!Arguments->TryGetStringField(TEXT("sound_asset_path"), SoundPath) || SoundPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("attach_audio_component: missing sound_asset_path"));
	}
	FString LoadError;
	USoundBase* Sound = ClaireonAudioApplyHelpers::LoadSoundBase(SoundPath, LoadError);
	if (!Sound) return MakeErrorResult(LoadError);

	AActor* Target = ClaireonAudioApplyHelpers::FindActorByLabel(World, ActorName);
	if (!Target) return MakeErrorResult(FString::Printf(TEXT("Actor '%s' not found in editor world"), *ActorName));

	bool bAutoActivate = true;
	Arguments->TryGetBoolField(TEXT("auto_activate"), bAutoActivate);

	FString ComponentName;
	if (!Arguments->TryGetStringField(TEXT("component_name"), ComponentName) || ComponentName.IsEmpty())
	{
		auto HasCompNamed = [Target](const FString& Name) -> bool
		{
			for (UActorComponent* C : Target->GetComponents())
			{
				if (C && C->GetName() == Name) return true;
			}
			return false;
		};
		for (int32 Next = 0; Next <= 1024; ++Next)
		{
			const FString Candidate = FString::Printf(TEXT("AudioComponent_%d"), Next);
			if (!HasCompNamed(Candidate))
			{
				ComponentName = Candidate;
				break;
			}
		}
		if (ComponentName.IsEmpty()) ComponentName = TEXT("AudioComponent_claireon");
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Attach Audio Component")));
	Target->Modify();

	UAudioComponent* AC = NewObject<UAudioComponent>(Target, *ComponentName, RF_Transactional);
	if (!AC) return MakeErrorResult(TEXT("NewObject<UAudioComponent> failed"));

	if (USceneComponent* Root = Target->GetRootComponent())
	{
		AC->AttachToComponent(Root, FAttachmentTransformRules::KeepRelativeTransform);
	}
	AC->RegisterComponent();
	Target->AddInstanceComponent(AC);
	AC->SetSound(Sound);
	AC->bAutoActivate = bAutoActivate;
	Target->MarkPackageDirty();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("component_name"), ComponentName);
	return MakeSuccessResult(Data, FString::Printf(TEXT("Attached %s to %s"), *ComponentName, *ActorName));
}
