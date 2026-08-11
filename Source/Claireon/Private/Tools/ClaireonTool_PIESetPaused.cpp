// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIESetPaused.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"

namespace ClaireonPIESetPausedInternal
{
	// File-local discriminator per project convention on anonymous-namespace collisions under
	// unity batching.
	static UWorld* Cl625SetPaused_FindPIEWorld()
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
}

FString ClaireonTool_PIESetPaused::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIESetPaused::GetOperation() const { return TEXT("set_paused"); }

FString ClaireonTool_PIESetPaused::GetDescription() const
{
	return TEXT("Set the PIE pause state explicitly (does NOT toggle). Prefer this over "
				"console_execute(command='pause'), which toggles and can silently leave PIE paused -- a paused world "
				"then looks like 'nothing is happening'. Reports both the previous and the new state, so a no-op is "
				"visible. Requires a live PIE session; errors when no PIE world is active.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIESetPaused::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> PausedProp = MakeShared<FJsonObject>();
	PausedProp->SetStringField(TEXT("type"), TEXT("boolean"));
	PausedProp->SetStringField(TEXT("description"),
		TEXT("Target pause state. true pauses, false resumes. Absolute, not a toggle: issuing the "
			 "same value twice is a no-op rather than a flip."));
	Properties->SetObjectField(TEXT("paused"), PausedProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("paused")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIESetPaused::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}

	bool bTargetPaused = false;
	if (!Arguments->TryGetBoolField(TEXT("paused"), bTargetPaused))
	{
		return MakeErrorResult(
			TEXT("Missing required field: paused (boolean). This tool sets an absolute state; "
				 "there is deliberately no toggle mode."));
	}

	UWorld* PIEWorld = ClaireonPIESetPausedInternal::Cl625SetPaused_FindPIEWorld();
	if (!IsValid(PIEWorld))
	{
		return MakeErrorResult(
			TEXT("pie_set_paused requires an active PIE session. Start PIE first with pie_start_async."));
	}

	// Read the prior state before changing it, so the response distinguishes "I paused it" from
	// "it was already paused" -- the ambiguity that made the toggling console command dangerous.
	const bool bWasPaused = PIEWorld->IsPaused();

	// SetGamePaused routes through the player controller, which is what actually owns pause
	// state; writing the world flag directly would not survive the next PC update.
	const bool bApplied = UGameplayStatics::SetGamePaused(PIEWorld, bTargetPaused);
	const bool bNowPaused = PIEWorld->IsPaused();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetBoolField(TEXT("requested"), bTargetPaused);
	Data->SetBoolField(TEXT("was_paused"), bWasPaused);
	Data->SetBoolField(TEXT("paused"), bNowPaused);
	Data->SetBoolField(TEXT("changed"), bWasPaused != bNowPaused);

	if (bNowPaused != bTargetPaused)
	{
		// SetGamePaused returns false when there is no player controller to accept it, e.g. a
		// dedicated-server PIE world. Report it rather than claiming success.
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to set PIE pause state to %s (SetGamePaused returned %s; world reports %s). "
				 "This usually means the PIE world has no player controller to own pause state."),
			bTargetPaused ? TEXT("true") : TEXT("false"),
			bApplied ? TEXT("true") : TEXT("false"),
			bNowPaused ? TEXT("paused") : TEXT("running")));
	}

	const FString Summary = (bWasPaused == bNowPaused)
		? FString::Printf(TEXT("PIE already %s; no change."), bNowPaused ? TEXT("paused") : TEXT("running"))
		: FString::Printf(TEXT("PIE %s (was %s)."),
			bNowPaused ? TEXT("paused") : TEXT("resumed"),
			bWasPaused ? TEXT("paused") : TEXT("running"));

	UE_LOG(LogClaireon, Log, TEXT("[pie_set_paused] %s"), *Summary);
	return MakeSuccessResult(Data, Summary);
}
