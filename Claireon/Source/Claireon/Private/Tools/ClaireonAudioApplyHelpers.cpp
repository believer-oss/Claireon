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
	namespace
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
		if (!Sound)
		{
			OutError = FString::Printf(TEXT("Failed to load USoundBase at '%s' (or wrong class)"), *Resolved.ResolvedPath.Path);
		}
		return Sound;
	}

	AActor* FindActorByLabel(UWorld* World, const FString& Label)
	{
		if (!World) return nullptr;
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
}
