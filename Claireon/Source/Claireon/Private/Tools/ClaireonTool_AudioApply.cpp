// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AudioApply.h"

#include "ClaireonPathResolver.h"
#include "Tools/ClaireonPropertyResolver.h"
#include "Tools/ClaireonPropertyUtils.h"

#include "Components/AudioComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "ScopedTransaction.h"
#include "Sound/AmbientSound.h"
#include "Sound/AudioVolume.h"
#include "Sound/SoundBase.h"
#include "UObject/UObjectGlobals.h"

using FToolResult = IClaireonTool::FToolResult;

namespace
{
	FVector AudioApply_ParseVector(const TSharedPtr<FJsonObject>& Obj, double DX = 0.0, double DY = 0.0, double DZ = 0.0)
	{
		double X = DX, Y = DY, Z = DZ;
		if (Obj.IsValid())
		{
			Obj->TryGetNumberField(TEXT("x"), X);
			Obj->TryGetNumberField(TEXT("y"), Y);
			Obj->TryGetNumberField(TEXT("z"), Z);
		}
		return FVector(X, Y, Z);
	}

	FRotator AudioApply_ParseRotator(const TSharedPtr<FJsonObject>& Obj)
	{
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		if (Obj.IsValid())
		{
			Obj->TryGetNumberField(TEXT("pitch"), Pitch);
			Obj->TryGetNumberField(TEXT("yaw"), Yaw);
			Obj->TryGetNumberField(TEXT("roll"), Roll);
		}
		return FRotator(Pitch, Yaw, Roll);
	}

	bool ParseTransformField(const TSharedPtr<FJsonObject>& Args, FTransform& OutXform, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* XformObj = nullptr;
		if (!Args->TryGetObjectField(TEXT("transform"), XformObj) || !XformObj)
		{
			OutError = TEXT("Missing required field: transform");
			return false;
		}

		const TSharedPtr<FJsonObject>* LocObj = nullptr;
		(*XformObj)->TryGetObjectField(TEXT("location"), LocObj);
		const FVector Location = AudioApply_ParseVector(LocObj ? *LocObj : nullptr);

		const TSharedPtr<FJsonObject>* RotObj = nullptr;
		(*XformObj)->TryGetObjectField(TEXT("rotation"), RotObj);
		const FRotator Rotation = AudioApply_ParseRotator(RotObj ? *RotObj : nullptr);

		const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
		(*XformObj)->TryGetObjectField(TEXT("scale"), ScaleObj);
		const FVector Scale = AudioApply_ParseVector(ScaleObj ? *ScaleObj : nullptr, 1.0, 1.0, 1.0);

		OutXform = FTransform(Rotation, Location, Scale);
		return true;
	}

	USoundBase* LoadSoundBase(const FString& AssetPath, FString& OutError)
	{
		auto Resolved = ClaireonPathResolver::Resolve(AssetPath);
		if (!Resolved.bSuccess)
		{
			OutError = FString::Printf(TEXT("Could not resolve sound asset path '%s': %s"), *AssetPath, *Resolved.Error);
			return nullptr;
		}
		USoundBase* Sound = LoadObject<USoundBase>(nullptr, *Resolved.ResolvedPath.Path);
		if (!Sound)
		{
			OutError = FString::Printf(TEXT("Failed to load USoundBase at '%s' (or wrong class)"), *Resolved.ResolvedPath.Path);
		}
		return Sound;
	}

	AActor* FindActorByLabel(UWorld* World, const FString& Label)
	{
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (A && A->GetActorLabel() == Label)
			{
				return A;
			}
		}
		return nullptr;
	}

	/** Reflection-write a properties blob onto a target UObject. Each value is stringified for
	 *  ClaireonPropertyResolver (matches PlaceActor behavior). */
	void WriteReflectedProperties(UObject* Target, const TSharedPtr<FJsonObject>& Props, TArray<FString>& OutWarnings)
	{
		if (!Target || !Props.IsValid()) return;
		for (const auto& Pair : Props->Values)
		{
			FString ValueStr;
			if (!Pair.Value->TryGetString(ValueStr))
			{
				double NumVal;
				bool BoolVal;
				if (Pair.Value->TryGetNumber(NumVal)) ValueStr = FString::SanitizeFloat(NumVal);
				else if (Pair.Value->TryGetBool(BoolVal)) ValueStr = BoolVal ? TEXT("True") : TEXT("False");
			}
			FString Err;
			bool bOk = false;
			if (AActor* AsActor = Cast<AActor>(Target))
			{
				ClaireonPropertyResolver::FResolvedProperty Resolved;
				bOk = ClaireonPropertyResolver::WritePropertyOnActor(AsActor, Pair.Key, ValueStr, Resolved, Err);
			}
			else
			{
				bOk = ClaireonPropertyUtils::WritePropertyByPath(Target, Pair.Key, ValueStr, Err);
			}
			if (!bOk)
			{
				OutWarnings.Add(FString::Printf(TEXT("Could not set %s: %s"), *Pair.Key, *Err));
			}
		}
	}
} // namespace

FString FClaireonTool_AudioApply::GetCategory() const { return TEXT("audio"); }
FString FClaireonTool_AudioApply::GetOperation() const { return TEXT("apply"); }

FString FClaireonTool_AudioApply::GetDescription() const
{
	return TEXT("Level-scoped audio operations on the current editor world: place ambient sound "
				"actors, place audio volumes, attach audio components, and reflection-write "
				"properties on audio actors/components. Requires an editor world and no active PIE. "
				"Every op wraps in FScopedTransaction so editor undo works.");
}

TSharedPtr<FJsonObject> FClaireonTool_AudioApply::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> OpProp = MakeShared<FJsonObject>();
	OpProp->SetStringField(TEXT("type"), TEXT("string"));
	OpProp->SetStringField(TEXT("description"),
		TEXT("Operation: 'place_ambient_sound', 'place_audio_volume', 'attach_audio_component', or 'set_audio_property'."));
	{
		TArray<TSharedPtr<FJsonValue>> EnumVals;
		EnumVals.Add(MakeShared<FJsonValueString>(TEXT("place_ambient_sound")));
		EnumVals.Add(MakeShared<FJsonValueString>(TEXT("place_audio_volume")));
		EnumVals.Add(MakeShared<FJsonValueString>(TEXT("attach_audio_component")));
		EnumVals.Add(MakeShared<FJsonValueString>(TEXT("set_audio_property")));
		OpProp->SetArrayField(TEXT("enum"), EnumVals);
	}
	Properties->SetObjectField(TEXT("operation"), OpProp);

	// Per-op fields are loose-typed: transform / sound_asset_path / actor_name / component_path / field_name / value / properties / label / auto_activate
	for (const TCHAR* Field : { TEXT("sound_asset_path"), TEXT("actor_name"), TEXT("component_path"),
								TEXT("field_name"), TEXT("component_name"), TEXT("label") })
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
	for (const TCHAR* Field : { TEXT("transform"), TEXT("properties") })
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("type"), TEXT("object"));
		Properties->SetObjectField(Field, P);
	}
	{
		// value -- any type
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		Properties->SetObjectField(TEXT("value"), P);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult FClaireonTool_AudioApply::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor || !GEditor->GetEditorWorldContext().World())
	{
		return MakeErrorResult(TEXT("audio_apply requires an active editor world"));
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();

	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments object missing"));
	}

	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: operation"));
	}

	if (Operation == TEXT("place_ambient_sound"))
	{
		FString SoundPath;
		if (!Arguments->TryGetStringField(TEXT("sound_asset_path"), SoundPath) || SoundPath.IsEmpty())
		{
			return MakeErrorResult(TEXT("place_ambient_sound: missing sound_asset_path"));
		}
		FString LoadError;
		USoundBase* Sound = LoadSoundBase(SoundPath, LoadError);
		if (!Sound) return MakeErrorResult(LoadError);

		FTransform Xform;
		FString XformError;
		if (!ParseTransformField(Arguments, Xform, XformError)) return MakeErrorResult(XformError);

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

	if (Operation == TEXT("place_audio_volume"))
	{
		FTransform Xform;
		FString XformError;
		if (!ParseTransformField(Arguments, Xform, XformError)) return MakeErrorResult(XformError);

		FString Label;
		Arguments->TryGetStringField(TEXT("label"), Label);

		FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Place Audio Volume")));

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		AAudioVolume* Actor = World->SpawnActor<AAudioVolume>(AAudioVolume::StaticClass(),
			Xform.GetLocation(), Xform.GetRotation().Rotator(), Params);
		if (!Actor) return MakeErrorResult(TEXT("Failed to spawn AAudioVolume"));

		Actor->SetActorScale3D(Xform.GetScale3D());
		if (!Label.IsEmpty()) Actor->SetActorLabel(Label, /*bMarkDirty=*/true);

		TArray<FString> Warnings;
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		if (Arguments->TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
		{
			WriteReflectedProperties(Actor, *PropsObj, Warnings);
		}
		Actor->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("actor_name"), Actor->GetActorLabel());
		Data->SetStringField(TEXT("actor_path"), Actor->GetPathName());
		FToolResult Result = MakeSuccessResult(Data, FString::Printf(TEXT("Placed AudioVolume %s"), *Actor->GetActorLabel()));
		Result.Warnings = MoveTemp(Warnings);
		return Result;
	}

	if (Operation == TEXT("attach_audio_component"))
	{
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
		USoundBase* Sound = LoadSoundBase(SoundPath, LoadError);
		if (!Sound) return MakeErrorResult(LoadError);

		AActor* Target = FindActorByLabel(World, ActorName);
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

	if (Operation == TEXT("set_audio_property"))
	{
		FString ActorName, ComponentPath, FieldName;
		Arguments->TryGetStringField(TEXT("actor_name"), ActorName);
		Arguments->TryGetStringField(TEXT("component_path"), ComponentPath);
		Arguments->TryGetStringField(TEXT("field_name"), FieldName);

		if (FieldName.IsEmpty())
		{
			return MakeErrorResult(TEXT("set_audio_property: missing field_name"));
		}
		if (ActorName.IsEmpty() && ComponentPath.IsEmpty())
		{
			return MakeErrorResult(TEXT("set_audio_property: one of actor_name or component_path is required"));
		}
		if (!ActorName.IsEmpty() && !ComponentPath.IsEmpty())
		{
			return MakeErrorResult(TEXT("set_audio_property: supply exactly one of actor_name or component_path, not both"));
		}

		// "value" field is any JSON value; stringify for ClaireonPropertyResolver.
		FString ValueStr;
		TSharedPtr<FJsonValue> ValueJson = Arguments->TryGetField(TEXT("value"));
		if (ValueJson.IsValid())
		{
			if (!ValueJson->TryGetString(ValueStr))
			{
				double NumVal;
				bool BoolVal;
				if (ValueJson->TryGetNumber(NumVal)) ValueStr = FString::SanitizeFloat(NumVal);
				else if (ValueJson->TryGetBool(BoolVal)) ValueStr = BoolVal ? TEXT("True") : TEXT("False");
			}
		}

		FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Audio Property")));

		if (!ActorName.IsEmpty())
		{
			AActor* Target = FindActorByLabel(World, ActorName);
			if (!Target) return MakeErrorResult(FString::Printf(TEXT("Actor '%s' not found"), *ActorName));
			Target->Modify();
			ClaireonPropertyResolver::FResolvedProperty Resolved;
			FString Err;
			if (!ClaireonPropertyResolver::WritePropertyOnActor(Target, FieldName, ValueStr, Resolved, Err))
			{
				return MakeErrorResult(Err);
			}
			Target->MarkPackageDirty();

			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("actor_name"), ActorName);
			Data->SetStringField(TEXT("field_name"), FieldName);
			return MakeSuccessResult(Data, FString::Printf(TEXT("Set %s on %s"), *FieldName, *ActorName));
		}

		// component_path: expect "ActorLabel.ComponentName"
		FString ActorPart, CompPart;
		if (!ComponentPath.Split(TEXT("."), &ActorPart, &CompPart) || ActorPart.IsEmpty() || CompPart.IsEmpty())
		{
			return MakeErrorResult(TEXT("component_path must be in the form '<actor_label>.<component_name>'"));
		}
		AActor* Target = FindActorByLabel(World, ActorPart);
		if (!Target) return MakeErrorResult(FString::Printf(TEXT("Actor '%s' not found"), *ActorPart));
		UActorComponent* TargetComp = nullptr;
		for (UActorComponent* Comp : Target->GetComponents())
		{
			if (Comp && Comp->GetName() == CompPart)
			{
				TargetComp = Comp;
				break;
			}
		}
		if (!TargetComp) return MakeErrorResult(FString::Printf(TEXT("Component '%s' not found on actor '%s'"), *CompPart, *ActorPart));

		TargetComp->Modify();
		FString Err;
		if (!ClaireonPropertyUtils::WritePropertyByPath(TargetComp, FieldName, ValueStr, Err))
		{
			return MakeErrorResult(Err);
		}
		Target->MarkPackageDirty();

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("component_path"), ComponentPath);
		Data->SetStringField(TEXT("field_name"), FieldName);
		return MakeSuccessResult(Data, FString::Printf(TEXT("Set %s on %s"), *FieldName, *ComponentPath));
	}

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation '%s'"), *Operation));
}
