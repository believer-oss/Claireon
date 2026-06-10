// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIETick.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

namespace ClaireonToolPIETickInternal
{
	// File-local discriminator `Cl625PIETick_` per project memory about
	// anonymous-namespace name collisions under unity batching.
	static UWorld* Cl625PIETick_FindPIEWorld()
	{
		if (!GEngine) { return nullptr; }
		for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
		{
			if (WorldContext.WorldType == EWorldType::PIE && WorldContext.World())
			{
				return WorldContext.World();
			}
		}
		return nullptr;
	}

	// Shared body: wall-clock sleep that fails when no PIE world is active.
	// Captures pre- and post-tick world time so callers can observe the in-game
	// delta (which may differ from wall-clock under time-scale changes).
	static IClaireonTool::FToolResult Cl625PIETick_ExecuteCore(
		const TSharedPtr<FJsonObject>& Arguments, const TCHAR* OpName)
	{
		double Seconds = 0.0;
		if (!Arguments->TryGetNumberField(TEXT("seconds"), Seconds))
		{
			return IClaireonTool::MakeErrorResult(TEXT("Missing required field: seconds"));
		}
		Seconds = FMath::Clamp(Seconds, 0.0, 300.0);

		UWorld* PIEWorld = Cl625PIETick_FindPIEWorld();
		if (!PIEWorld)
		{
			return IClaireonTool::MakeErrorResult(
				FString::Printf(TEXT("%s requires an active PIE session. Start PIE first with pie.start_async."), OpName));
		}

		const double WallStart = FPlatformTime::Seconds();
		const double TimeStart = PIEWorld->GetTimeSeconds();

		const double PollIntervalSec = 0.05;
		while (FPlatformTime::Seconds() - WallStart < Seconds)
		{
			FPlatformProcess::Sleep(static_cast<float>(
				FMath::Min(PollIntervalSec, Seconds - (FPlatformTime::Seconds() - WallStart))));
		}

		const double WallElapsed = FPlatformTime::Seconds() - WallStart;
		const double WorldElapsed = PIEWorld->GetTimeSeconds() - TimeStart;

		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetNumberField(TEXT("requested_seconds"), Seconds);
		Data->SetNumberField(TEXT("wall_elapsed_seconds"), WallElapsed);
		Data->SetNumberField(TEXT("world_elapsed_seconds"), WorldElapsed);
		Data->SetNumberField(TEXT("world_time_start"), TimeStart);
		Data->SetNumberField(TEXT("world_time_end"), PIEWorld->GetTimeSeconds());

		const FString Summary = FString::Printf(
			TEXT("%s: world advanced %.3fs (wall %.3fs, requested %.3fs)"),
			OpName, WorldElapsed, WallElapsed, Seconds);
		return IClaireonTool::MakeSuccessResult(Data, Summary);
	}

	static TSharedPtr<FJsonObject> Cl625PIETick_BuildSchema()
	{
		TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
		Schema->SetStringField(TEXT("type"), TEXT("object"));

		TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

		TSharedPtr<FJsonObject> SecondsProp = MakeShared<FJsonObject>();
		SecondsProp->SetStringField(TEXT("type"), TEXT("number"));
		SecondsProp->SetStringField(TEXT("description"),
			TEXT("Wall-clock seconds to wait. Clamped to [0.0, 300.0]. Returns world-time delta separately."));
		Properties->SetObjectField(TEXT("seconds"), SecondsProp);

		Schema->SetObjectField(TEXT("properties"), Properties);

		TArray<TSharedPtr<FJsonValue>> Required;
		Required.Add(MakeShared<FJsonValueString>(TEXT("seconds")));
		Schema->SetArrayField(TEXT("required"), Required);

		return Schema;
	}
}

// ---- ClaireonTool_PIETick ----

FString ClaireonTool_PIETick::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIETick::GetOperation() const { return TEXT("pie_tick"); }

FString ClaireonTool_PIETick::GetDescription() const
{
	return TEXT("Advance the active PIE simulation by N wall-clock seconds. Errors when no PIE world is active. "
		"Use this when you want to give the game a chance to tick without leaving Python (the engine keeps ticking during the wait). "
		"Returns wall-clock and world-time deltas separately so callers can detect time-scale changes.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIETick::GetInputSchema() const
{
	return ClaireonToolPIETickInternal::Cl625PIETick_BuildSchema();
}

IClaireonTool::FToolResult ClaireonTool_PIETick::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	return ClaireonToolPIETickInternal::Cl625PIETick_ExecuteCore(Arguments, TEXT("pie_tick"));
}

// ---- ClaireonTool_PIESleep ----

FString ClaireonTool_PIESleep::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIESleep::GetOperation() const { return TEXT("pie_sleep"); }

FString ClaireonTool_PIESleep::GetDescription() const
{
	return TEXT("Semantic alias for pie_tick: sleep N wall-clock seconds while the PIE world keeps ticking. "
		"Errors when no PIE world is active. Prefer this name when the operator intent is \"let the simulation run for a moment\".");
}

TSharedPtr<FJsonObject> ClaireonTool_PIESleep::GetInputSchema() const
{
	return ClaireonToolPIETickInternal::Cl625PIETick_BuildSchema();
}

IClaireonTool::FToolResult ClaireonTool_PIESleep::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	return ClaireonToolPIETickInternal::Cl625PIETick_ExecuteCore(Arguments, TEXT("pie_sleep"));
}
