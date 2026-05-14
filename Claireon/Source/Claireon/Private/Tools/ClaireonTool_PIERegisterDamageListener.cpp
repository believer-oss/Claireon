// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIERegisterDamageListener.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

FString ClaireonTool_PIERegisterDamageListener::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIERegisterDamageListener::GetOperation() const { return TEXT("register_damage_listener"); }

FString ClaireonTool_PIERegisterDamageListener::GetDescription() const
{
	return TEXT("Register a damage event listener on an actor's health component. "
		"Returns a listener ID that can be used with getDamageEvents and unregisterDamageListener.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIERegisterDamageListener::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// actorId - required
	TSharedPtr<FJsonObject> ActorIdProp = MakeShared<FJsonObject>();
	ActorIdProp->SetStringField(TEXT("type"), TEXT("string"));
	ActorIdProp->SetStringField(TEXT("description"),
		TEXT("The stable actor ID to attach the damage listener to (e.g., 'actor_0')"));
	Properties->SetObjectField(TEXT("actorId"), ActorIdProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIERegisterDamageListener::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.pie.registerDamageListener"));

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor is not available"));
	}

	if (!GEditor->IsPlaySessionInProgress())
	{
		return MakeErrorResult(TEXT("PIE is not running. Start a PIE session first with editor.pie.start"));
	}

	// Parse required parameter
	if (!Arguments.IsValid() || !Arguments->HasField(TEXT("actorId")))
	{
		return MakeErrorResult(TEXT("Missing required parameter: actorId"));
	}

	const FString ActorId = Arguments->GetStringField(TEXT("actorId"));

	// Find PIE world
	UWorld* PIEWorld = nullptr;
	for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
	{
		if (WorldContext.WorldType == EWorldType::PIE && WorldContext.World())
		{
			PIEWorld = WorldContext.World();
			break;
		}
	}

	if (!PIEWorld)
	{
		return MakeErrorResult(TEXT("PIE world not found. PIE may still be initializing."));
	}

	// Resolve the actor
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	AActor* Actor = PIEManager.ResolveActorId(ActorId, PIEWorld);

	if (!Actor)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Could not resolve actor '%s'. The actor may have been destroyed or the ID is stale."),
			*ActorId));
	}

	// List available health-related components for diagnostics
	FString HealthComponentInfo;
	TArray<UActorComponent*> Components;
	Actor->GetComponents(Components);

	for (const UActorComponent* Component : Components)
	{
		if (Component && Component->GetClass()->GetName().Contains(TEXT("Health")))
		{
			HealthComponentInfo += FString::Printf(TEXT("  - %s (%s)\n"),
				*Component->GetName(), *Component->GetClass()->GetName());
		}
	}

	// Register the damage listener
	const FString ListenerId = PIEManager.RegisterDamageListener(Actor);

	if (ListenerId.IsEmpty())
	{
		FString ErrorMsg = FString::Printf(
			TEXT("Failed to register damage listener on actor '%s' (%s). "),
			*ActorId, *Actor->GetClass()->GetName());
		ErrorMsg += TEXT("No health component was found on this actor. ");
		ErrorMsg += TEXT("The actor may not have a health component, or it may use a non-standard naming convention.\n");
		ErrorMsg += TEXT("Components on this actor:\n");

		for (const UActorComponent* Component : Components)
		{
			if (Component)
			{
				ErrorMsg += FString::Printf(TEXT("  - %s (%s)\n"),
					*Component->GetName(), *Component->GetClass()->GetName());
			}
		}

		return MakeErrorResult(ErrorMsg);
	}

	// Build output
	FString Output;
	Output += FString::Printf(TEXT("listenerId: %s\n"), *ListenerId);
	Output += FString::Printf(TEXT("actorId: %s\n"), *ActorId);
	Output += FString::Printf(TEXT("actorClass: %s\n"), *Actor->GetClass()->GetName());

	if (!HealthComponentInfo.IsEmpty())
	{
		Output += TEXT("healthComponents:\n");
		Output += HealthComponentInfo;
	}

	Output += TEXT("status: registered\n");
	Output += TEXT("Note: Use editor.pie.getDamageEvents with this listenerId to retrieve damage events. ");
	Output += TEXT("Use editor.pie.unregisterDamageListener to clean up when done.\n");

	return MakeSuccessResult(nullptr, Output);
}
