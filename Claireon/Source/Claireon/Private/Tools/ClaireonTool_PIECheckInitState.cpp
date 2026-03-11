// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIECheckInitState.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Components/GameFrameworkInitStateInterface.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#if WITH_LYRA_GAME
#include "LyraGameplayTags.h"
#include "Character/LyraPawnExtensionComponent.h"
#endif

FString ClaireonTool_PIECheckInitState::GetName() const
{
	return TEXT("editor.pie.checkInitState");
}

FString ClaireonTool_PIECheckInitState::GetDescription() const
{
	return TEXT("Check the Lyra initialization state of an actor in the PIE world. Reports which init state the actor has reached (Spawned, DataAvailable, DataInitialized, GameplayReady).");
}

TSharedPtr<FJsonObject> ClaireonTool_PIECheckInitState::GetInputSchema() const
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

	// initState - optional, if specified we check if this specific state has been reached
	TSharedPtr<FJsonObject> InitStateProp = MakeShared<FJsonObject>();
	InitStateProp->SetStringField(TEXT("type"), TEXT("string"));
	InitStateProp->SetStringField(TEXT("description"),
		TEXT("Target init state to check for (optional). If specified, returns whether the actor has reached this state."));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("InitState.Spawned")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("InitState.DataAvailable")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("InitState.DataInitialized")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("InitState.GameplayReady")));
		InitStateProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("initState"), InitStateProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("actorId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

#if WITH_LYRA_GAME
namespace
{
	/** Map a user-friendly state string to the corresponding gameplay tag. */
	FGameplayTag ResolveInitStateTag(const FString& StateName)
	{
		if (StateName == TEXT("InitState.Spawned") || StateName == TEXT("Spawned"))
		{
			return LyraGameplayTags::InitState_Spawned;
		}
		if (StateName == TEXT("InitState.DataAvailable") || StateName == TEXT("DataAvailable"))
		{
			return LyraGameplayTags::InitState_DataAvailable;
		}
		if (StateName == TEXT("InitState.DataInitialized") || StateName == TEXT("DataInitialized"))
		{
			return LyraGameplayTags::InitState_DataInitialized;
		}
		if (StateName == TEXT("InitState.GameplayReady") || StateName == TEXT("GameplayReady"))
		{
			return LyraGameplayTags::InitState_GameplayReady;
		}
		return FGameplayTag();
	}

	/** Determine the highest init state reached by the PawnExtensionComponent.
	 *  Checks from highest to lowest and returns the first match. */
	FString DetermineCurrentInitState(const ULyraPawnExtensionComponent* PawnExt)
	{
		// Check in descending order â return the highest reached state
		if (PawnExt->HasReachedInitState(LyraGameplayTags::InitState_GameplayReady))
		{
			return TEXT("InitState.GameplayReady");
		}
		if (PawnExt->HasReachedInitState(LyraGameplayTags::InitState_DataInitialized))
		{
			return TEXT("InitState.DataInitialized");
		}
		if (PawnExt->HasReachedInitState(LyraGameplayTags::InitState_DataAvailable))
		{
			return TEXT("InitState.DataAvailable");
		}
		if (PawnExt->HasReachedInitState(LyraGameplayTags::InitState_Spawned))
		{
			return TEXT("InitState.Spawned");
		}
		return TEXT("(none)");
	}
}
#endif // WITH_LYRA_GAME

IClaireonTool::FToolResult ClaireonTool_PIECheckInitState::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if !WITH_LYRA_GAME
	return MakeErrorResult(TEXT("Init state checking requires Lyra integration. This build does not include LyraGame."));
#else
	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.pie.checkInitState"));

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor is not available"));
	}

	if (!GEditor->IsPlaySessionInProgress())
	{
		return MakeErrorResult(TEXT("PIE is not running. Start a PIE session first with editor.pie.start"));
	}

	// Parse parameters
	if (!Arguments.IsValid() || !Arguments->HasField(TEXT("actorId")))
	{
		return MakeErrorResult(TEXT("Missing required parameter: actorId"));
	}

	const FString ActorId = Arguments->GetStringField(TEXT("actorId"));
	FString TargetStateName;
	if (Arguments->HasField(TEXT("initState")))
	{
		TargetStateName = Arguments->GetStringField(TEXT("initState"));
	}

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
			TEXT("Actor '%s' not found or has been destroyed."), *ActorId));
	}

	// Find PawnExtensionComponent — this is the Lyra component that implements init state
	ULyraPawnExtensionComponent* PawnExt = ULyraPawnExtensionComponent::FindPawnExtensionComponent(Actor);
	if (!PawnExt)
	{
		// Actor does not have a PawnExtensionComponent — init state system not applicable
		FString Output;
		Output += FString::Printf(TEXT("actorId: %s\n"), *ActorId);
		Output += FString::Printf(TEXT("className: %s\n"), *Actor->GetClass()->GetName());
		Output += TEXT("hasInitState: false\n");
		Output += TEXT("Note: This actor does not have a ULyraPawnExtensionComponent. ");
		Output += TEXT("The Lyra init state system only applies to actors with PawnExtension components ");
		Output += TEXT("(typically player characters and AI pawns).\n");

		// List components that implement IGameFrameworkInitStateInterface for diagnostic purposes
		TArray<UActorComponent*> Components;
		Actor->GetComponents(Components);
		bool bFoundAny = false;
		for (UActorComponent* Component : Components)
		{
			if (Component && Cast<IGameFrameworkInitStateInterface>(Component))
			{
				if (!bFoundAny)
				{
					Output += TEXT("initStateComponents:\n");
					bFoundAny = true;
				}
				Output += FString::Printf(TEXT("  - %s (%s)\n"),
					*Component->GetName(), *Component->GetClass()->GetName());
			}
		}

		return MakeSuccessResult(nullptr, Output);
	}

	// Determine the current init state
	const FString CurrentState = DetermineCurrentInitState(PawnExt);

	// Build output
	FString Output;
	Output += FString::Printf(TEXT("actorId: %s\n"), *ActorId);
	Output += FString::Printf(TEXT("className: %s\n"), *Actor->GetClass()->GetName());
	Output += TEXT("hasInitState: true\n");
	Output += FString::Printf(TEXT("currentState: %s\n"), *CurrentState);

	// If a target state was specified, check if it has been reached
	if (!TargetStateName.IsEmpty())
	{
		const FGameplayTag TargetTag = ResolveInitStateTag(TargetStateName);
		if (!TargetTag.IsValid())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Invalid init state name: '%s'. Valid values: InitState.Spawned, InitState.DataAvailable, InitState.DataInitialized, InitState.GameplayReady"),
				*TargetStateName));
		}

		const bool bHasReached = PawnExt->HasReachedInitState(TargetTag);
		Output += FString::Printf(TEXT("targetState: %s\n"), *TargetStateName);
		Output += FString::Printf(TEXT("hasReached: %s\n"), bHasReached ? TEXT("true") : TEXT("false"));
	}

	// Always list the available states and their status for full visibility
	Output += TEXT("availableStates:\n");
	Output += FString::Printf(TEXT("  - InitState.Spawned: %s\n"),
		PawnExt->HasReachedInitState(LyraGameplayTags::InitState_Spawned) ? TEXT("reached") : TEXT("not reached"));
	Output += FString::Printf(TEXT("  - InitState.DataAvailable: %s\n"),
		PawnExt->HasReachedInitState(LyraGameplayTags::InitState_DataAvailable) ? TEXT("reached") : TEXT("not reached"));
	Output += FString::Printf(TEXT("  - InitState.DataInitialized: %s\n"),
		PawnExt->HasReachedInitState(LyraGameplayTags::InitState_DataInitialized) ? TEXT("reached") : TEXT("not reached"));
	Output += FString::Printf(TEXT("  - InitState.GameplayReady: %s\n"),
		PawnExt->HasReachedInitState(LyraGameplayTags::InitState_GameplayReady) ? TEXT("reached") : TEXT("not reached"));

	return MakeSuccessResult(nullptr, Output);
#endif // WITH_LYRA_GAME
}
