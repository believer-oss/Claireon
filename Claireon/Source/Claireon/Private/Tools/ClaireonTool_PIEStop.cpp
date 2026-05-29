// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIEStop.h"
#include "ClaireonBridge.h"
#include "ClaireonLog.h"
#include "ClaireonPIEManager.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

FString ClaireonTool_PIEStop::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIEStop::GetOperation() const { return TEXT("stop_async"); }

FString ClaireonTool_PIEStop::GetDescription() const
{
	return TEXT("Stop the current Play In Editor (PIE) session (calls RequestEndPlayMap). "
		"Use this when a tool reports 'cannot be used while PIE is running'. "
		"The PIE stop is deferred until after the current script finishes.");
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

	double DurationSeconds = (FDateTime::UtcNow() - Session->StartTime).GetTotalSeconds();

	FClaireonBridge::EnqueueDeferredAction({
		EClaireonDeferredActionType::PIEStop,
		FString()  // no payload needed
	});

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("deferred"));
	Data->SetStringField(TEXT("action"), TEXT("pie_stop"));
	Data->SetNumberField(TEXT("duration_seconds"), DurationSeconds);

	FString Summary = FString::Printf(TEXT("PIE stop queued (ran %.1fs) — executes after script completes"), DurationSeconds);
	return MakeSuccessResult(Data, Summary);
}

void ClaireonTool_PIEStop::ExecuteDeferredPIEStop()
{
	if (!GEditor)
	{
		return;
	}

	FClaireonPIEManager& PIEManager = FClaireonPIEManager::Get();
	PIEManager.RestoreThrottleCPU();
	GEditor->RequestEndPlayMap();

	UE_LOG(LogClaireon, Log, TEXT("ExecuteDeferredPIEStop: PIE stop requested"));
}
