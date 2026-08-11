// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEWaitFor.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"
#include "Tools/ClaireonWaitSupport.h"

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
#include "HAL/PlatformTime.h"
#if WITH_LYRA_GAME
#include "LyraGameplayTags.h"
#endif
#include "Misc/Timespan.h"

FString ClaireonTool_PIEWaitFor::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIEWaitFor::GetOperation() const { return TEXT("wait_for"); }

FString ClaireonTool_PIEWaitFor::GetDescription() const
{
    return TEXT("Wait for a PIE condition (map load, PIE ready, actor valid, init state, duration) without blocking the editor. Returns immediately if the condition already holds; otherwise returns {status:'waiting', wait_id} and re-checks once per frame. Poll the wait_id (via this tool or pie_wait_poll) for the terminal state. Stateless / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEWaitFor::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// condition - required unless wait_id is provided
	TSharedPtr<FJsonObject> ConditionProp = MakeShared<FJsonObject>();
	ConditionProp->SetStringField(TEXT("type"), TEXT("string"));
	ConditionProp->SetStringField(TEXT("description"),
		TEXT("The condition to wait for: 'mapLoad' (editor map loaded), 'pieReady' (PIE world has begun play), 'actorValid' (actor ID resolves to a valid actor), 'initState' (actor has reached a Lyra init state), 'duration' (wait for a fixed amount of time). Required unless wait_id is provided."));
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
		TEXT("Maximum time the registered wait may stay pending before it reports timedOut (default: 30, clamped to [0.1, 300])"));
	TimeoutProp->SetNumberField(TEXT("default"), 30.0);
	Properties->SetObjectField(TEXT("timeoutSeconds"), TimeoutProp);

	// pollIntervalMs - accepted for backward compatibility, no longer used
	TSharedPtr<FJsonObject> PollIntervalProp = MakeShared<FJsonObject>();
	PollIntervalProp->SetStringField(TEXT("type"), TEXT("number"));
	PollIntervalProp->SetStringField(TEXT("description"),
		TEXT("DEPRECATED and ignored: the wait is now non-blocking and the condition is re-checked once per editor frame. Accepted for backward compatibility."));
	PollIntervalProp->SetNumberField(TEXT("default"), 100.0);
	Properties->SetObjectField(TEXT("pollIntervalMs"), PollIntervalProp);

	// wait_id - poll a wait started by a previous call
	TSharedPtr<FJsonObject> WaitIdProp = MakeShared<FJsonObject>();
	WaitIdProp->SetStringField(TEXT("type"), TEXT("string"));
	WaitIdProp->SetStringField(TEXT("description"),
		TEXT("Poll mode: the wait_id returned by a previous call that answered {status:'waiting'}. When present, all other parameters are ignored and the wait's current/terminal state is reported (terminal state is consumed on read)."));
	Properties->SetObjectField(TEXT("wait_id"), WaitIdProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// No JSON-schema-level required fields: 'condition' is required unless
	// 'wait_id' is provided, which a static required list cannot express.
	// The implementation enforces the conditional requirement.
	Schema->SetArrayField(TEXT("required"), TArray<TSharedPtr<FJsonValue>>());

	return Schema;
}

namespace ClaireonTool_PIEWaitFor_Private
{
	/** Helper: find PIE world from engine contexts */
	UWorld* FindPIEWorld()
	{
		if (!IsValid(GEngine))
		{
			return nullptr;
		}
		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.WorldType == EWorldType::PIE && IsValid(WorldContext.World()))
			{
				return WorldContext.World();
			}
		}
		return nullptr;
	}

#if WITH_LYRA_GAME
	/** Map a user-friendly state string to the corresponding gameplay tag.
	 *  Named uniquely to avoid collision with the identical function in ClaireonTool_PIEPIEWaitFor_CheckInitState.cpp
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
	bool PIEWaitFor_CheckMapLoad()
	{
		if (!IsValid(GEditor))
		{
			return false;
		}
		const FWorldContext* EditorContext = nullptr;
		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.WorldType == EWorldType::Editor && IsValid(WorldContext.World()))
			{
				EditorContext = &WorldContext;
				break;
			}
		}
		return EditorContext != nullptr && EditorContext->World() != nullptr;
	}

	/** Check the "pieReady" condition: PIE world exists and HasBegunPlay */
	bool PIEWaitFor_CheckPIEReady()
	{
		if (!IsValid(GEditor) || !GEditor->IsPlaySessionInProgress())
		{
			return false;
		}
		UWorld* PIEWorld = FindPIEWorld();
		return IsValid(PIEWorld) && PIEWorld->HasBegunPlay();
	}

	/** Check the "actorValid" condition: actor ID resolves to a valid actor */
	bool PIEWaitFor_CheckActorValid(const FString& ActorId)
	{
		if (ActorId.IsEmpty())
		{
			return false;
		}
		UWorld* PIEWorld = FindPIEWorld();
		if (!IsValid(PIEWorld))
		{
			return false;
		}
		AActor* Actor = FClaireonPIEManager::Get().ResolveActorId(ActorId, PIEWorld);
		return Actor != nullptr;
	}

#if WITH_LYRA_GAME
	/** Check the "initState" condition: actor has reached specified init state */
	bool PIEWaitFor_CheckInitState(const FString& ActorId, const FGameplayTag& TargetTag)
	{
		if (ActorId.IsEmpty() || !TargetTag.IsValid())
		{
			return false;
		}
		UWorld* PIEWorld = FindPIEWorld();
		if (!IsValid(PIEWorld))
		{
			return false;
		}
		AActor* Actor = FClaireonPIEManager::Get().ResolveActorId(ActorId, PIEWorld);
		if (!IsValid(Actor))
		{
			return false;
		}
		ULyraPawnExtensionComponent* PawnExt = ULyraPawnExtensionComponent::FindPawnExtensionComponent(Actor);
		if (!IsValid(PawnExt))
		{
			return false;
		}
		return PawnExt->HasReachedInitState(TargetTag);
	}
#endif // WITH_LYRA_GAME

	/** Build the condition-specific timeout diagnostics block. Evaluated at
	 *  the moment the wait times out so it reports live state. */
	FString PIEWaitFor_BuildTimeoutDiagnostics(
		const FString& Condition, const FString& ParamActorId, const FString& ParamInitState)
	{
		FString Diagnostics;

		if (Condition == TEXT("pieReady"))
		{
			const bool bSessionActive = IsValid(GEditor) && GEditor->IsPlaySessionInProgress();
			UWorld* PIEWorld = FindPIEWorld();
			Diagnostics += FString::Printf(TEXT("diagnostics.sessionActive: %s\n"),
				bSessionActive ? TEXT("true") : TEXT("false"));
			Diagnostics += FString::Printf(TEXT("diagnostics.pieWorldExists: %s\n"),
				IsValid(PIEWorld) ? TEXT("true") : TEXT("false"));
			if (IsValid(PIEWorld))
			{
				Diagnostics += FString::Printf(TEXT("diagnostics.hasBegunPlay: %s\n"),
					PIEWorld->HasBegunPlay() ? TEXT("true") : TEXT("false"));
			}
		}
		else if (Condition == TEXT("actorValid"))
		{
			Diagnostics += FString::Printf(TEXT("diagnostics.actorId: %s\n"), *ParamActorId);
		}
		else if (Condition == TEXT("initState"))
		{
			Diagnostics += FString::Printf(TEXT("diagnostics.actorId: %s\n"), *ParamActorId);
			Diagnostics += FString::Printf(TEXT("diagnostics.targetState: %s\n"), *ParamInitState);
#if WITH_LYRA_GAME
			// Try to report current state if actor is valid
			UWorld* PIEWorld = FindPIEWorld();
			if (IsValid(PIEWorld))
			{
				AActor* Actor = FClaireonPIEManager::Get().ResolveActorId(ParamActorId, PIEWorld);
				if (IsValid(Actor))
				{
					ULyraPawnExtensionComponent* PawnExt = ULyraPawnExtensionComponent::FindPawnExtensionComponent(Actor);
					if (IsValid(PawnExt))
					{
						// Report which states have been reached
						Diagnostics += FString::Printf(TEXT("diagnostics.spawned: %s\n"),
							PawnExt->HasReachedInitState(LyraGameplayTags::InitState_Spawned) ? TEXT("true") : TEXT("false"));
						Diagnostics += FString::Printf(TEXT("diagnostics.dataAvailable: %s\n"),
							PawnExt->HasReachedInitState(LyraGameplayTags::InitState_DataAvailable) ? TEXT("true") : TEXT("false"));
						Diagnostics += FString::Printf(TEXT("diagnostics.dataInitialized: %s\n"),
							PawnExt->HasReachedInitState(LyraGameplayTags::InitState_DataInitialized) ? TEXT("true") : TEXT("false"));
						Diagnostics += FString::Printf(TEXT("diagnostics.gameplayReady: %s\n"),
							PawnExt->HasReachedInitState(LyraGameplayTags::InitState_GameplayReady) ? TEXT("true") : TEXT("false"));
					}
					else
					{
						Diagnostics += TEXT("diagnostics.note: Actor does not have a PawnExtensionComponent\n");
					}
				}
				else
				{
					Diagnostics += TEXT("diagnostics.note: Actor ID could not be resolved\n");
				}
			}
#endif // WITH_LYRA_GAME
		}

		return Diagnostics;
	}
}
using namespace ClaireonTool_PIEWaitFor_Private;

IClaireonTool::FToolResult ClaireonTool_PIEWaitFor::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.pie.waitFor"));

	// Poll mode: a wait_id from a previous {status:'waiting'} response.
	// Handled before the GEditor gate because polling only touches the wait
	// registry (the condition checks themselves are null-safe).
	if (Arguments.IsValid())
	{
		FString WaitId;
		if (Arguments->TryGetStringField(TEXT("wait_id"), WaitId) && !WaitId.IsEmpty())
		{
			return ClaireonWaitSupport::BuildWaitPollResult(FClaireonPIEWaitRegistry::Get(), WaitId);
		}
	}

	if (!IsValid(GEditor))
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

	// pollIntervalMs is accepted for backward compatibility but no longer
	// used: the wait is non-blocking and re-checked once per editor frame.

	// Clamp values to sensible ranges
	TimeoutSeconds = FMath::Clamp(TimeoutSeconds, 0.1, 300.0);

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
#if WITH_LYRA_GAME
	FGameplayTag InitStateTargetTag;
#endif
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
		InitStateTargetTag = ResolveInitStateTagForWait(ParamInitState);
		if (!InitStateTargetTag.IsValid())
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

	UE_LOG(LogClaireon, Display, TEXT("[MCP] waitFor: condition='%s', timeout=%.1fs"),
		*Condition, TimeoutSeconds);

	// Build the per-frame condition check. All checks are game-thread and
	// null-safe against PIE ending between frames.
	TFunction<bool()> ConditionCheck;
	if (Condition == TEXT("mapLoad"))
	{
		ConditionCheck = []() { return PIEWaitFor_CheckMapLoad(); };
	}
	else if (Condition == TEXT("pieReady"))
	{
		ConditionCheck = []() { return PIEWaitFor_CheckPIEReady(); };
	}
	else if (Condition == TEXT("actorValid"))
	{
		ConditionCheck = [ParamActorId]() { return PIEWaitFor_CheckActorValid(ParamActorId); };
	}
	else if (Condition == TEXT("initState"))
	{
#if WITH_LYRA_GAME
		ConditionCheck = [ParamActorId, InitStateTargetTag]()
		{
			return PIEWaitFor_CheckInitState(ParamActorId, InitStateTargetTag);
		};
#endif // WITH_LYRA_GAME
	}
	else // duration
	{
		const double DurationStartSeconds = FPlatformTime::Seconds();
		const double DurationSeconds = ParamDurationSeconds;
		ConditionCheck = [DurationStartSeconds, DurationSeconds]()
		{
			return FPlatformTime::Seconds() - DurationStartSeconds >= DurationSeconds;
		};
	}

	if (!ConditionCheck)
	{
		// Defensive: every validated condition assigns a check above.
		return MakeErrorResult(FString::Printf(
			TEXT("Internal error: no condition check could be built for condition '%s'"), *Condition));
	}

	TFunction<FString()> DiagnosticsProvider =
		[Condition, ParamActorId, ParamInitState]()
	{
		return PIEWaitFor_BuildTimeoutDiagnostics(Condition, ParamActorId, ParamInitState);
	};

	// Non-blocking design (replaces the old game-thread sleep-poll, which
	// starved the very ticks the conditions needed to become true): check the
	// condition immediately; if not yet met, register a frame-ticked wait and
	// hand back a wait_id for the caller to poll.
	const ClaireonWaitSupport::FStartWaitOutcome Outcome = ClaireonWaitSupport::StartOrCompleteWait(
		FClaireonPIEWaitRegistry::Get(), Condition, MoveTemp(ConditionCheck), TimeoutSeconds, MoveTemp(DiagnosticsProvider));

	if (Outcome.bImmediatelyMet)
	{
		TSharedPtr<FJsonObject> MetData = MakeShared<FJsonObject>();
		MetData->SetStringField(TEXT("status"), TEXT("met"));
		MetData->SetStringField(TEXT("condition"), Condition);
		MetData->SetBoolField(TEXT("conditionMet"), true);
		MetData->SetBoolField(TEXT("timedOut"), false);
		MetData->SetBoolField(TEXT("immediate"), true);
		MetData->SetNumberField(TEXT("elapsedSeconds"), 0.0);
		MetData->SetNumberField(TEXT("timeoutSeconds"), TimeoutSeconds);

		FString Output;
		Output += FString::Printf(TEXT("condition: %s\n"), *Condition);
		Output += TEXT("conditionMet: true\n");
		Output += TEXT("timedOut: false\n");
		Output += TEXT("elapsedSeconds: 0.000\n");
		Output += FString::Printf(TEXT("timeoutSeconds: %.1f\n"), TimeoutSeconds);
		Output += TEXT("immediate: true\n");

		UE_LOG(LogClaireon, Display, TEXT("[MCP] waitFor result: condition='%s' already met, no wait registered"),
			*Condition);

		return MakeSuccessResult(MetData, Output);
	}

	TSharedPtr<FJsonObject> WaitingData = MakeShared<FJsonObject>();
	WaitingData->SetStringField(TEXT("status"), TEXT("waiting"));
	WaitingData->SetStringField(TEXT("wait_id"), Outcome.WaitId);
	WaitingData->SetStringField(TEXT("condition"), Condition);
	WaitingData->SetNumberField(TEXT("timeoutSeconds"), TimeoutSeconds);

	FString Output;
	Output += FString::Printf(TEXT("condition: %s\n"), *Condition);
	Output += TEXT("status: waiting\n");
	Output += FString::Printf(TEXT("waitId: %s\n"), *Outcome.WaitId);
	Output += FString::Printf(TEXT("timeoutSeconds: %.1f\n"), TimeoutSeconds);
	Output += FString::Printf(
		TEXT("Note: Condition '%s' is not yet met. The wait is re-checked once per editor frame without blocking. ")
		TEXT("Poll pie_wait_for (or pie_wait_poll) with {\"wait_id\": \"%s\"} to retrieve the terminal state; ")
		TEXT("it reports timedOut after %.1f seconds if the condition never holds.\n"),
		*Condition, *Outcome.WaitId, TimeoutSeconds);

	UE_LOG(LogClaireon, Display, TEXT("[MCP] waitFor: condition='%s' not yet met, registered wait %s (timeout %.1fs)"),
		*Condition, *Outcome.WaitId, TimeoutSeconds);

	return MakeSuccessResult(WaitingData, Output);
}
