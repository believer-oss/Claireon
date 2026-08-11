// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAudioApplyHelpers.h"
#include "Tools/ClaireonPropertyResolver.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "ClaireonPathResolver.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Sound/SoundBase.h"
#include "UObject/UObjectGlobals.h"

namespace ClaireonAudioApplyHelpers
{
	namespace ClaireonAudioApplyHelpers_Private
	{
		// File-local helpers avoid anon-namespace symbol collisions under unity batching.
		static FVector AudioApplyHelpers_ParseVector(const TSharedPtr<FJsonObject>& Obj, double DX = 0.0, double DY = 0.0, double DZ = 0.0)
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

		static FRotator AudioApplyHelpers_ParseRotator(const TSharedPtr<FJsonObject>& Obj)
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
	}
	using namespace ClaireonAudioApplyHelpers_Private;

	bool ParseTransformField(const TSharedPtr<FJsonObject>& Args, FTransform& OutXform, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* XformObj = nullptr;
		if (!Args.IsValid() || !Args->TryGetObjectField(TEXT("transform"), XformObj) || !XformObj)
		{
			OutError = TEXT("Missing required field: transform");
			return false;
		}

		const TSharedPtr<FJsonObject>* LocObj = nullptr;
		(*XformObj)->TryGetObjectField(TEXT("location"), LocObj);
		const FVector Location = AudioApplyHelpers_ParseVector(LocObj ? *LocObj : nullptr);

		const TSharedPtr<FJsonObject>* RotObj = nullptr;
		(*XformObj)->TryGetObjectField(TEXT("rotation"), RotObj);
		const FRotator Rotation = AudioApplyHelpers_ParseRotator(RotObj ? *RotObj : nullptr);

		const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
		(*XformObj)->TryGetObjectField(TEXT("scale"), ScaleObj);
		const FVector Scale = AudioApplyHelpers_ParseVector(ScaleObj ? *ScaleObj : nullptr, 1.0, 1.0, 1.0);

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
		if (!IsValid(Sound))
		{
			OutError = FString::Printf(TEXT("Failed to load USoundBase at '%s' (or wrong class)"), *Resolved.ResolvedPath.Path);
		}
		return Sound;
	}

	AActor* FindActorByLabel(UWorld* World, const FString& Label)
	{
		if (!IsValid(World)) return nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* A = *It;
			if (IsValid(A) && A->GetActorLabel() == Label)
			{
				return A;
			}
		}
		return nullptr;
	}

	void WriteReflectedProperties(UObject* Target, const TSharedPtr<FJsonObject>& Props, TArray<FString>& OutWarnings)
	{
		if (!IsValid(Target) || !Props.IsValid()) return;
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
			if (AActor* AsActor = Cast<AActor>(Target); IsValid(AsActor))
			{
				ClaireonPropertyResolver::FResolvedProperty Resolved;
				bOk = ClaireonPropertyResolver::WritePropertyOnActor(AsActor, FString(*Pair.Key), ValueStr, Resolved, Err);
			}
			else
			{
				bOk = ClaireonPropertyUtils::WritePropertyByPath(Target, FString(*Pair.Key), ValueStr, Err);
			}
			if (!bOk)
			{
				OutWarnings.Add(FString::Printf(TEXT("Could not set %s: %s"), *Pair.Key, *Err));
			}
		}
	}
}

namespace ClaireonAudioSchema
{
	namespace
	{
		// File-local prefix to avoid anon-NS collisions under unity batching.
		TSharedPtr<FJsonObject> Cl612Audio_MakeProp(const TCHAR* Description)
		{
			TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
			Prop->SetStringField(TEXT("description"), Description);
			return Prop;
		}
	}

	void AddString(const TSharedPtr<FJsonObject>& Properties, const TCHAR* Name, const TCHAR* Description)
	{
		TSharedPtr<FJsonObject> Prop = Cl612Audio_MakeProp(Description);
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Properties->SetObjectField(Name, Prop);
	}

	void AddBoolean(const TSharedPtr<FJsonObject>& Properties, const TCHAR* Name, const TCHAR* Description)
	{
		TSharedPtr<FJsonObject> Prop = Cl612Audio_MakeProp(Description);
		Prop->SetStringField(TEXT("type"), TEXT("boolean"));
		Properties->SetObjectField(Name, Prop);
	}

	void AddObject(const TSharedPtr<FJsonObject>& Properties, const TCHAR* Name, const TCHAR* Description)
	{
		TSharedPtr<FJsonObject> Prop = Cl612Audio_MakeProp(Description);
		Prop->SetStringField(TEXT("type"), TEXT("object"));
		Properties->SetObjectField(Name, Prop);
	}

	void AddAnyType(const TSharedPtr<FJsonObject>& Properties, const TCHAR* Name, const TCHAR* Description)
	{
		TSharedPtr<FJsonObject> Prop = Cl612Audio_MakeProp(Description);
		// JSON Schema type-array form. Stating the accepted set is honest;
		// omitting "type" entirely is indistinguishable from forgetting it.
		TArray<TSharedPtr<FJsonValue>> Types;
		for (const TCHAR* T : { TEXT("string"), TEXT("number"), TEXT("boolean"), TEXT("object"), TEXT("array") })
		{
			Types.Add(MakeShared<FJsonValueString>(T));
		}
		Prop->SetArrayField(TEXT("type"), Types);
		Properties->SetObjectField(Name, Prop);
	}
}
