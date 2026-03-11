// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEGetActor.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

FString ClaireonTool_PIEGetActor::GetName() const
{
	return TEXT("pie_get_actor");
}

FString ClaireonTool_PIEGetActor::GetDescription() const
{
	return TEXT("Look up an actor by its stable ID in the PIE world. Actor IDs are assigned by tools like getPlayerPawnByPlayerIndex.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEGetActor::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// actorId - required
	TSharedPtr<FJsonObject> ActorIdProp = MakeShared<FJsonObject>();
	ActorIdProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorIdProp->SetStringField(TEXT("description"),
		TEXT("The stable actor ID assigned by PIE tools (e.g., 'actor_0')"));
	Properties->SetObjectField(TEXT("actorId"), ActorIdProp);

	// includeDetails - optional, default false
	TSharedPtr<FJsonObject> IncludeDetailsProp = MakeShared<FJsonObject>();
	IncludeDetailsProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeDetailsProp->SetStringField(TEXT("description"),
		TEXT("If true, include location, rotation, and component list in the response"));
	IncludeDetailsProp->SetBoolField(TEXT("default"), false);
	Properties->SetObjectField(TEXT("includeDetails"), IncludeDetailsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIEGetActor::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString ActorId;
	if (!Arguments->TryGetStringField(TEXT("actorId"), ActorId))
	{
		return MakeErrorResult(TEXT("Missing required argument: actorId"));
	}

	bool bIncludeDetails = false;
	Arguments->TryGetBoolField(TEXT("includeDetails"), bIncludeDetails);

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

	// Resolve the actor ID
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	AActor* Actor = PIEManager.ResolveActorId(ActorId, PIEWorld);
	if (!Actor)
	{
		return MakeErrorResult(FString::Printf(TEXT("Actor not found for ID: %s"), *ActorId));
	}

	FString ActorName = Actor->GetName();
	FString ActorClass = Actor->GetClass()->GetName();
	FVector Location = Actor->GetActorLocation();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("name"), ActorName);
	Data->SetStringField(TEXT("class"), ActorClass);

	TSharedPtr<FJsonObject> LocationObj = MakeShared<FJsonObject>();
	LocationObj->SetNumberField(TEXT("x"), Location.X);
	LocationObj->SetNumberField(TEXT("y"), Location.Y);
	LocationObj->SetNumberField(TEXT("z"), Location.Z);
	Data->SetObjectField(TEXT("location"), LocationObj);

	if (bIncludeDetails)
	{
		// Add rotation
		FRotator Rotation = Actor->GetActorRotation();
		TSharedPtr<FJsonObject> RotationObj = MakeShared<FJsonObject>();
		RotationObj->SetNumberField(TEXT("pitch"), Rotation.Pitch);
		RotationObj->SetNumberField(TEXT("yaw"), Rotation.Yaw);
		RotationObj->SetNumberField(TEXT("roll"), Rotation.Roll);
		Data->SetObjectField(TEXT("rotation"), RotationObj);

		// Add component list
		TArray<TSharedPtr<FJsonValue>> ComponentsArray;
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		for (UActorComponent* Comp : Components)
		{
			if (Comp)
			{
				ComponentsArray.Add(MakeShared<FJsonValueString>(
					FString::Printf(TEXT("%s (%s)"), *Comp->GetName(), *Comp->GetClass()->GetName())));
			}
		}
		Data->SetArrayField(TEXT("components"), ComponentsArray);

		TSharedPtr<FJsonObject> PropertiesObj = MakeShared<FJsonObject>();
		PropertiesObj->SetBoolField(TEXT("is_hidden"), Actor->IsHidden());
		PropertiesObj->SetBoolField(TEXT("can_tick"), Actor->PrimaryActorTick.bCanEverTick);
		Data->SetObjectField(TEXT("properties"), PropertiesObj);
	}

	FString Summary = FString::Printf(TEXT("Found %s at (%.0f, %.0f, %.0f)"),
		*ActorName, Location.X, Location.Y, Location.Z);

	return MakeSuccessResult(Data, Summary);
}
