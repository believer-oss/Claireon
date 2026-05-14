// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEStatus.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"

FString ClaireonTool_PIEStatus::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIEStatus::GetOperation() const { return TEXT("status"); }

FString ClaireonTool_PIEStatus::GetDescription() const
{
	return TEXT("Get the status of the current Play In Editor (PIE) session including uptime and game state");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEStatus::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIEStatus::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	const FClaireonPIEManager::FPIESession* Session = PIEManager.GetActiveSession();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();

	if (!Session)
	{
		Data->SetBoolField(TEXT("active"), false);
		Data->SetStringField(TEXT("map"), TEXT(""));
		Data->SetNumberField(TEXT("player_count"), 0);
		Data->SetNumberField(TEXT("frame_rate"), 0.0);
		Data->SetNumberField(TEXT("uptime_seconds"), 0.0);

		return MakeSuccessResult(Data, TEXT("PIE is not active"));
	}

	// Get uptime
	double UptimeSeconds = (FDateTime::UtcNow() - Session->StartTime).GetTotalSeconds();

	// Get player count and frame rate from PIE world
	int32 PlayerCount = 0;
	float FrameRate = 0.0f;

	if (GEditor)
	{
		UWorld* PIEWorld = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World())
			{
				PIEWorld = Context.World();
				break;
			}
		}

		if (PIEWorld)
		{
			PlayerCount = PIEWorld->GetNumPlayerControllers();

			// Get frame rate from the game thread delta time
			if (FApp::GetDeltaTime() > 0.0)
			{
				FrameRate = static_cast<float>(1.0 / FApp::GetDeltaTime());
			}
		}
	}

	Data->SetBoolField(TEXT("active"), true);
	Data->SetStringField(TEXT("map"), Session->MapPath);
	Data->SetNumberField(TEXT("player_count"), PlayerCount);
	Data->SetNumberField(TEXT("frame_rate"), FrameRate);
	Data->SetNumberField(TEXT("uptime_seconds"), UptimeSeconds);

	FString Summary = FString::Printf(
		TEXT("PIE active on %s (%.0f FPS, %.0fs uptime)"),
		*Session->MapPath,
		FrameRate,
		UptimeSeconds);

	return MakeSuccessResult(Data, Summary);
}
