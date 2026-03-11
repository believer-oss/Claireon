// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEWaitFor.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Components/GameFrameworkInitStateInterface.h"
#if WITH_LYRA_GAME
#include "Character/LyraPawnExtensionComponent.h"
#endif
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "HAL/PlatformProcess.h"
#if WITH_LYRA_GAME
#include "LyraGameplayTags.h"
#endif
#include "Misc/Timespan.h"

FString ClaireonTool_PIEWaitFor::GetName() const
{
	return TEXT("editor.pie.waitFor");
}

FString ClaireonTool_PIEWaitFor::GetDescription() const
{
	return TEXT("Wait for a condition to be met in the PIE session, with configurable timeout and poll interval. Blocks the response until the condition is met or timeout expires.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEWaitFor::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// condition - required
	TSharedPtr<FJsonObject> ConditionProp = MakeShared<FJsonObject>();
	ConditionProp->SetStringField(TEXT("type"), TEXT("string"));
	ConditionProp->SetStringField(TEXT("description"),
		TEXT("The condition to wait for: 'mapLoad' (editor map loaded), 'pieReady' (PIE world has begun play), 'actorValid' (actor ID resolves to a valid actor), 'initState' (actor has reached a Lyra init state), 'duration' (wait for a fixed amount of time)"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("mapLoad")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("pieReady")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("actorValid")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("initState")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("duration")));
		ConditionProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("condition"), ConditionProp);

	// conditionParams - optional object with condition-specific parameters
	TSharedPtr<FJsonObject> ConditionParamsProp = MakeShared<FJsonObject>();
	ConditionParamsProp->SetStringField(TEXT("type"), TEXT("object"));
	ConditionParamsProp->SetStringField(TEXT("description"),
		TEXT("Condition-specific parameters. For 'actorValid': {actorId: string}. For 'initState': {actorId: string, initState: string}. For 'duration': {seconds: number}."));
	Properties->SetObjectField(TEXT("conditionParams"), ConditionParamsProp);

	// timeoutSeconds - optional, default 30
	TSharedPtr<FJsonObject> TimeoutProp = MakeShared<FJsonObject>();
	TimeoutProp->SetStringField(TEXT("type"), TEXT("number"));
	TimeoutProp->SetStringField(TEXT("description"),
		TEXT("Maximum time to wait in seconds before timing out (default: 30)"));
	TimeoutProp->SetNumberField(TEXT("default"), 30.0);
	Properties->SetObjectField(TEXT("timeoutSeconds"), TimeoutProp);

	// pollIntervalMs - optional, default 100
	TSharedPtr<FJsonObject> PollIntervalProp = MakeShared<FJsonObject>();
	PollIntervalProp->SetStringField(TEXT("type"), TEXT("number"));
	PollIntervalProp->SetStringField(TEXT("description"),
		TEXT("Polling interval in milliseconds (default: 100)"));
	PollIntervalProp->SetNumberField(TEXT("default"), 100.0);
	Properties->SetObjectField(TEXT("pollIntervalMs"), PollIntervalProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("condition")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

namespace
{
	/** Helper: find PIE world from engine contexts */
	UWorld* FindPIEWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.WorldType == EWorldType::PIE && WorldContext.World())
			{
				return WorldContext.World();
			}
		}
		return nullptr;
	}

#if WITH_LYRA_GAME
	/** Map a user-friendly state string to the corresponding gameplay tag.
	 *  Named uniquely to avoid collision with the identical function in ClaireonTool_PIECheckInitState.cpp
	 *  during unity builds. */
	FGameplayTag ResolveInitStateTagForWait(const FString& StateName)
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
#endif // WITH_LYRA_GAME

	/** Check the "mapLoad" condition: editor world exists and is valid */
	bool CheckMapLoad()
	{
		if (!GEditor)
		{
			return false;
		}
		const FWorldContext* EditorContext = nullptr;
		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.WorldType == EWorldType::Editor && WorldContext.World())
			{
				EditorContext = &WorldContext;
				break;
			}
		}
		return EditorContext != nullptr && EditorContext->World() != nullptr;
	}

	/** Check the "pieReady" condition: PIE world exists and HasBegunPlay */
	bool CheckPIEReady()
	{
		if (!GEditor || !GEditor->IsPlaySessionInProgress())
		{
			return false;
		}
		UWorld* PIEWorld = FindPIEWorld();
		return PIEWorld && PIEWorld->HasBegunPlay();
	}

	/** Check the "actorValid" condition: actor ID resolves to a valid actor */
	bool CheckActorValid(const FString& ActorId)
	{
		if (ActorId.IsEmpty())
		{
			return false;
		}
		UWorld* PIEWorld = FindPIEWorld();
		if (!PIEWorld)
		{
			return false;
		}
		AActor* Actor = FClaireonPIEManager::Get().ResolveActorId(ActorId, PIEWorld);
		return Actor != nullptr;
	}

#if WITH_LYRA_GAME
	/** Check the "initState" condition: actor has reached specified init state */
	bool CheckInitState(const FString& ActorId, const FGameplayTag& TargetTag)
	{
		if (ActorId.IsEmpty() || !TargetTag.IsValid())
		{
			return false;
		}
		UWorld* PIEWorld = FindPIEWorld();
		if (!PIEWorld)
		{
			return false;
		}
		AActor* Actor = FClaireonPIEManager::Get().ResolveActorId(ActorId, PIEWorld);
		if (!Actor)
		{
			return false;
		}
		ULyraPawnExtensionComponent* PawnExt = ULyraPawnExtensionComponent::FindPawnExtensionComponent(Actor);
		if (!PawnExt)
		{
			return false;
		}
		return PawnExt->HasReachedInitState(TargetTag);
	}
#endif // WITH_LYRA_GAME
}

IClaireonTool::FToolResult ClaireonTool_PIEWaitFor::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.pie.waitFor"));

	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor is not available"));
	}

	// Parse parameters
	if (!Arguments.IsValid() || !Arguments->HasField(TEXT("condition")))
	{
		return MakeErrorResult(TEXT("Missing required parameter: condition"));
	}

	const FString Condition = Arguments->GetStringField(TEXT("condition"));

	double TimeoutSeconds = 30.0;
	if (Arguments->HasField(TEXT("timeoutSeconds")))
	{
		TimeoutSeconds = Arguments->GetNumberField(TEXT("timeoutSeconds"));
	}

	double PollIntervalMs = 100.0;
	if (Arguments->HasField(TEXT("pollIntervalMs")))
	{
		PollIntervalMs = Arguments->GetNumberField(TEXT("pollIntervalMs"));
	}

	// Clamp values to sensible ranges
	TimeoutSeconds = FMath::Clamp(TimeoutSeconds, 0.1, 300.0);
	PollIntervalMs = FMath::Clamp(PollIntervalMs, 10.0, 5000.0);

	// Parse conditionParams
	const TSharedPtr<FJsonObject>* ConditionParamsPtr = nullptr;
	TSharedPtr<FJsonObject> ConditionParams;
	if (Arguments->HasField(TEXT("conditionParams")))
	{
		Arguments->TryGetObjectField(TEXT("conditionParams"), ConditionParamsPtr);
		if (ConditionParamsPtr)
		{
			ConditionParams = *ConditionParamsPtr;
		}
	}

	// Extract condition-specific parameters
	FString ParamActorId;
	FString ParamInitState;
	double ParamDurationSeconds = 0.0;

	if (ConditionParams.IsValid())
	{
		ConditionParams->TryGetStringField(TEXT("actorId"), ParamActorId);
		ConditionParams->TryGetStringField(TEXT("initState"), ParamInitState);
		if (ConditionParams->HasField(TEXT("seconds")))
		{
			ParamDurationSeconds = ConditionParams->GetNumberField(TEXT("seconds"));
		}
	}

	// Validate condition type and required params
	if (Condition == TEXT("actorValid"))
	{
		if (ParamActorId.IsEmpty())
		{
			return MakeErrorResult(TEXT("Condition 'actorValid' requires conditionParams.actorId"));
		}
	}
	else if (Condition == TEXT("initState"))
	{
		if (ParamActorId.IsEmpty())
		{
			return MakeErrorResult(TEXT("Condition 'initState' requires conditionParams.actorId"));
		}
		if (ParamInitState.IsEmpty())
		{
			return MakeErrorResult(TEXT("Condition 'initState' requires conditionParams.initState"));
		}
#if !WITH_LYRA_GAME
		return MakeErrorResult(TEXT("Condition 'initState' requires Lyra integration. This build does not include LyraGame."));
#else
		// Validate the init state name
		const FGameplayTag TargetTag = ResolveInitStateTagForWait(ParamInitState);
		if (!TargetTag.IsValid())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Invalid init state: '%s'. Valid values: InitState.Spawned, InitState.DataAvailable, InitState.DataInitialized, InitState.GameplayReady"),
				*ParamInitState));
		}
#endif // WITH_LYRA_GAME
	}
	else if (Condition == TEXT("duration"))
	{
		if (ParamDurationSeconds <= 0.0)
		{
			// If no explicit seconds param, use the timeout as the duration
			ParamDurationSeconds = TimeoutSeconds;
		}
	}
	else if (Condition != TEXT("mapLoad") && Condition != TEXT("pieReady"))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Unknown condition: '%s'. Valid conditions: mapLoad, pieReady, actorValid, initState, duration"),
			*Condition));
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] waitFor: condition='%s', timeout=%.1fs, pollInterval=%.0fms"),
		*Condition, TimeoutSeconds, PollIntervalMs);

	// Synchronous polling loop
	// NOTE: This blocks the current thread (which is the game thread for MCP tool execution).
	// This will freeze the editor UI during the wait. This is acceptable for automated testing
	// scenarios where PIE tools are used. For interactive use, keep timeouts short.
	const double StartTime = FPlatformTime::Seconds();
	const double PollIntervalSec = PollIntervalMs / 1000.0;
	bool bConditionMet = false;

	while (FPlatformTime::Seconds() - StartTime < TimeoutSeconds)
	{
		// Check condition based on type
		if (Condition == TEXT("mapLoad"))
		{
			bConditionMet = CheckMapLoad();
		}
		else if (Condition == TEXT("pieReady"))
		{
			bConditionMet = CheckPIEReady();
		}
		else if (Condition == TEXT("actorValid"))
		{
			bConditionMet = CheckActorValid(ParamActorId);
		}
		else if (Condition == TEXT("initState"))
		{
#if WITH_LYRA_GAME
			const FGameplayTag TargetTag = ResolveInitStateTagForWait(ParamInitState);
			bConditionMet = CheckInitState(ParamActorId, TargetTag);
#endif
		}
		else if (Condition == TEXT("duration"))
		{
			// "duration" condition is met when the specified seconds have elapsed
			const double Elapsed = FPlatformTime::Seconds() - StartTime;
			bConditionMet = (Elapsed >= ParamDurationSeconds);
		}

		if (bConditionMet)
		{
			break;
		}

		FPlatformProcess::Sleep(static_cast<float>(PollIntervalSec));
	}

	const double ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
	const bool bTimedOut = !bConditionMet;

	// Build output
	FString Output;
	Output += FString::Printf(TEXT("condition: %s\n"), *Condition);
	Output += FString::Printf(TEXT("conditionMet: %s\n"), bConditionMet ? TEXT("true") : TEXT("false"));
	Output += FString::Printf(TEXT("timedOut: %s\n"), bTimedOut ? TEXT("true") : TEXT("false"));
	Output += FString::Printf(TEXT("elapsedSeconds: %.3f\n"), ElapsedSeconds);
	Output += FString::Printf(TEXT("timeoutSeconds: %.1f\n"), TimeoutSeconds);

	if (bTimedOut)
	{
		Output += FString::Printf(TEXT("Note: Condition '%s' was not met within %.1f seconds.\n"),
			*Condition, TimeoutSeconds);

		// Add diagnostic info for specific conditions
		if (Condition == TEXT("pieReady"))
		{
			const bool bSessionActive = GEditor && GEditor->IsPlaySessionInProgress();
			UWorld* PIEWorld = FindPIEWorld();
			Output += FString::Printf(TEXT("diagnostics.sessionActive: %s\n"),
				bSessionActive ? TEXT("true") : TEXT("false"));
			Output += FString::Printf(TEXT("diagnostics.pieWorldExists: %s\n"),
				PIEWorld ? TEXT("true") : TEXT("false"));
			if (PIEWorld)
			{
				Output += FString::Printf(TEXT("diagnostics.hasBegunPlay: %s\n"),
					PIEWorld->HasBegunPlay() ? TEXT("true") : TEXT("false"));
			}
		}
		else if (Condition == TEXT("actorValid"))
		{
			Output += FString::Printf(TEXT("diagnostics.actorId: %s\n"), *ParamActorId);
		}
		else if (Condition == TEXT("initState"))
		{
			Output += FString::Printf(TEXT("diagnostics.actorId: %s\n"), *ParamActorId);
			Output += FString::Printf(TEXT("diagnostics.targetState: %s\n"), *ParamInitState);
#if WITH_LYRA_GAME
			// Try to report current state if actor is valid
			UWorld* PIEWorld = FindPIEWorld();
			if (PIEWorld)
			{
				AActor* Actor = FClaireonPIEManager::Get().ResolveActorId(ParamActorId, PIEWorld);
				if (Actor)
				{
					ULyraPawnExtensionComponent* PawnExt = ULyraPawnExtensionComponent::FindPawnExtensionComponent(Actor);
					if (PawnExt)
					{
						// Report which states have been reached
						Output += FString::Printf(TEXT("diagnostics.spawned: %s\n"),
							PawnExt->HasReachedInitState(LyraGameplayTags::InitState_Spawned) ? TEXT("true") : TEXT("false"));
						Output += FString::Printf(TEXT("diagnostics.dataAvailable: %s\n"),
							PawnExt->HasReachedInitState(LyraGameplayTags::InitState_DataAvailable) ? TEXT("true") : TEXT("false"));
						Output += FString::Printf(TEXT("diagnostics.dataInitialized: %s\n"),
							PawnExt->HasReachedInitState(LyraGameplayTags::InitState_DataInitialized) ? TEXT("true") : TEXT("false"));
						Output += FString::Printf(TEXT("diagnostics.gameplayReady: %s\n"),
							PawnExt->HasReachedInitState(LyraGameplayTags::InitState_GameplayReady) ? TEXT("true") : TEXT("false"));
					}
					else
					{
						Output += TEXT("diagnostics.note: Actor does not have a PawnExtensionComponent\n");
					}
				}
				else
				{
					Output += TEXT("diagnostics.note: Actor ID could not be resolved\n");
				}
			}
#endif // WITH_LYRA_GAME
		}
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] waitFor result: condition='%s', met=%s, elapsed=%.3fs"),
		*Condition, bConditionMet ? TEXT("true") : TEXT("false"), ElapsedSeconds);

	return MakeSuccessResult(nullptr, Output);
}
