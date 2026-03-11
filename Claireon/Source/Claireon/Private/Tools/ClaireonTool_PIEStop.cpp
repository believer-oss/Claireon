// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEStop.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

FString ClaireonTool_PIEStop::GetName() const
{
	return TEXT("pie_stop");
}

FString ClaireonTool_PIEStop::GetDescription() const
{
	return TEXT("Stop the current Play In Editor (PIE) session");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEStop::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();
	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIEStop::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	const FClaireonPIEManager::FPIESession* Session = PIEManager.GetActiveSession();
	if (!Session)
	{
		return MakeErrorResult(TEXT("No active PIE session to stop"));
	}

	// Compute duration before stopping
	double DurationSeconds = (FDateTime::UtcNow() - Session->StartTime).GetTotalSeconds();

	// Restore CPU throttling
	PIEManager.RestoreThrottleCPU();

	// Request PIE stop
	GEditor->RequestEndPlayMap();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("stopped"));
	Data->SetNumberField(TEXT("duration_seconds"), DurationSeconds);

	FString Summary = FString::Printf(TEXT("PIE stopped after %.1fs"), DurationSeconds);
	return MakeSuccessResult(Data, Summary);
}
