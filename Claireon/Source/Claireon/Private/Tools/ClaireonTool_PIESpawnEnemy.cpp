// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIESpawnEnemy.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/Pawn.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

FString ClaireonTool_PIESpawnEnemy::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIESpawnEnemy::GetOperation() const { return TEXT("spawn_enemy"); }

FString ClaireonTool_PIESpawnEnemy::GetDescription() const
{
	return TEXT("Spawn an AI enemy from a PawnData asset path in the active PIE session. "
		"Can spawn at an explicit location, relative to another actor, or at the world origin.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIESpawnEnemy::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// pawnDataPath - required
	TSharedPtr<FJsonObject> PawnDataPathProp = MakeShared<FJsonObject>();
	PawnDataPathProp->SetStringField(TEXT("type"), TEXT("string"));
	PawnDataPathProp->SetStringField(TEXT("description"),
		TEXT("Asset path to the PawnData or character Blueprint to spawn (e.g., '/Game/Characters/Enemies/PD_Goblin.PD_Goblin')"));
	Properties->SetObjectField(TEXT("pawnDataPath"), PawnDataPathProp);

	// location - optional
	TSharedPtr<FJsonObject> LocationProp = MakeShared<FJsonObject>();
	LocationProp->SetStringField(TEXT("type"), TEXT("object"));
	LocationProp->SetStringField(TEXT("description"),
		TEXT("Explicit world location to spawn at. If omitted, uses relativeToActor or world origin."));
	{
		TSharedPtr<FJsonObject> LocProperties = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> XProp = MakeShared<FJsonObject>();
		XProp->SetStringField(TEXT("type"), TEXT("number"));
		LocProperties->SetObjectField(TEXT("x"), XProp);

		TSharedPtr<FJsonObject> YProp = MakeShared<FJsonObject>();
		YProp->SetStringField(TEXT("type"), TEXT("number"));
		LocProperties->SetObjectField(TEXT("y"), YProp);

		TSharedPtr<FJsonObject> ZProp = MakeShared<FJsonObject>();
		ZProp->SetStringField(TEXT("type"), TEXT("number"));
		LocProperties->SetObjectField(TEXT("z"), ZProp);

		LocationProp->SetObjectField(TEXT("properties"), LocProperties);
	}
	Properties->SetObjectField(TEXT("location"), LocationProp);

	// relativeToActor - optional
	TSharedPtr<FJsonObject> RelativeToActorProp = MakeShared<FJsonObject>();
	RelativeToActorProp->SetStringField(TEXT("type"), TEXT("string"));
	RelativeToActorProp->SetStringField(TEXT("description"),
		TEXT("Actor ID to spawn relative to. The enemy will spawn in front of this actor at the specified distance."));
	Properties->SetObjectField(TEXT("relativeToActor"), RelativeToActorProp);

	// distance - optional
	TSharedPtr<FJsonObject> DistanceProp = MakeShared<FJsonObject>();
	DistanceProp->SetStringField(TEXT("type"), TEXT("number"));
	DistanceProp->SetStringField(TEXT("description"),
		TEXT("Distance in front of relativeToActor to spawn at (default: 500 units)"));
	DistanceProp->SetNumberField(TEXT("default"), 500.0);
	Properties->SetObjectField(TEXT("distance"), DistanceProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("pawnDataPath")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIESpawnEnemy::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString PawnDataPath;
	if (!Arguments->TryGetStringField(TEXT("pawnDataPath"), PawnDataPath))
	{
		return MakeErrorResult(TEXT("Missing required argument: pawnDataPath"));
	}

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	// Find the PIE world
	UWorld* PIEWorld = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::PIE && Context.World())
		{
			PIEWorld = Context.World();
			break;
		}
	}

	if (!PIEWorld)
	{
		return MakeErrorResult(TEXT("No active PIE session"));
	}

	// Determine spawn location
	FVector SpawnLocation = FVector::ZeroVector;

	const TSharedPtr<FJsonObject>* LocationObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj)
	{
		double X = 0.0, Y = 0.0, Z = 0.0;
		(*LocationObj)->TryGetNumberField(TEXT("x"), X);
		(*LocationObj)->TryGetNumberField(TEXT("y"), Y);
		(*LocationObj)->TryGetNumberField(TEXT("z"), Z);
		SpawnLocation = FVector(X, Y, Z);
	}
	else
	{
		// Check relativeToActor
		FString RelativeActorId;
		if (Arguments->TryGetStringField(TEXT("relativeToActor"), RelativeActorId) && !RelativeActorId.IsEmpty())
		{
			FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
			AActor* RelActor = PIEManager.ResolveActorId(RelativeActorId, PIEWorld);
			if (RelActor)
			{
				double Distance = 500.0;
				Arguments->TryGetNumberField(TEXT("distance"), Distance);

				FVector Forward = RelActor->GetActorForwardVector();
				SpawnLocation = RelActor->GetActorLocation() + Forward * static_cast<float>(Distance);
			}
		}
	}

	// Resolve pawn data path
	{
		auto ResolveResult = ClaireonPathResolver::Resolve(PawnDataPath);
		if (ResolveResult.bSuccess)
		{
			PawnDataPath = ResolveResult.ResolvedPath.Path;
		}
	}

	// Load the blueprint class from asset path
	UClass* SpawnClass = nullptr;

	// Try loading as a Blueprint
	UObject* LoadedObject = StaticLoadObject(UObject::StaticClass(), nullptr, *PawnDataPath);
	if (LoadedObject)
	{
		if (UBlueprint* BP = Cast<UBlueprint>(LoadedObject))
		{
			SpawnClass = BP->GeneratedClass;
		}
		else if (UClass* DirectClass = Cast<UClass>(LoadedObject))
		{
			SpawnClass = DirectClass;
		}
	}

	if (!SpawnClass)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Could not load spawn class from: %s"), *PawnDataPath));
	}

	// Validate it's a pawn/character/actor
	if (!SpawnClass->IsChildOf(AActor::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Class %s is not an Actor subclass"), *SpawnClass->GetName()));
	}

	// Spawn the actor
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

	AActor* SpawnedActor = PIEWorld->SpawnActor<AActor>(SpawnClass, SpawnLocation, FRotator::ZeroRotator, SpawnParams);
	if (!SpawnedActor)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to spawn actor of class: %s"), *SpawnClass->GetName()));
	}

	// Register actor ID
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	FString ActorId = PIEManager.GetActorId(SpawnedActor);

	FString ActorName = SpawnedActor->GetName();
	FString ActorClass = SpawnedActor->GetClass()->GetName();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("actor_name"), ActorName);
	Data->SetStringField(TEXT("actor_id"), ActorId);
	Data->SetStringField(TEXT("class"), ActorClass);

	TSharedPtr<FJsonObject> LocationResult = MakeShared<FJsonObject>();
	LocationResult->SetNumberField(TEXT("x"), SpawnLocation.X);
	LocationResult->SetNumberField(TEXT("y"), SpawnLocation.Y);
	LocationResult->SetNumberField(TEXT("z"), SpawnLocation.Z);
	Data->SetObjectField(TEXT("location"), LocationResult);

	FString Summary = FString::Printf(TEXT("Spawned %s at (%.0f, %.0f, %.0f)"),
		*ActorClass, SpawnLocation.X, SpawnLocation.Y, SpawnLocation.Z);

	return MakeSuccessResult(Data, Summary);
}
