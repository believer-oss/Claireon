// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_WaitSeconds.h"
#include "ClaireonLog.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

FString ClaireonTool_WaitSeconds::GetCategory() const { return TEXT("editor"); }
FString ClaireonTool_WaitSeconds::GetOperation() const { return TEXT("wait_seconds"); }

FString ClaireonTool_WaitSeconds::GetDescription() const
{
	return TEXT("Sleep for a wall-clock duration while keeping the editor tick alive. "
		"Use this instead of `time.sleep` (which is intercepted with a RuntimeWarning because it freezes the editor). "
		"The tool blocks for `seconds` wall-clock then returns; FTSTicker keeps deferred actions, async loads, and the editor UI responsive.");
}

TSharedPtr<FJsonObject> ClaireonTool_WaitSeconds::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> SecondsProp = MakeShared<FJsonObject>();
	SecondsProp->SetStringField(TEXT("type"), TEXT("number"));
	SecondsProp->SetStringField(TEXT("description"),
		TEXT("Wall-clock seconds to wait. Clamped to [0.0, 300.0]; sub-second values are honoured."));
	Properties->SetObjectField(TEXT("seconds"), SecondsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("seconds")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_WaitSeconds::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	double Seconds = 0.0;
	if (!Arguments->TryGetNumberField(TEXT("seconds"), Seconds))
	{
		return MakeErrorResult(TEXT("Missing required field: seconds"));
	}
	Seconds = FMath::Clamp(Seconds, 0.0, 300.0);

	const double StartTime = FPlatformTime::Seconds();
	// Poll-sleep pattern shared with ClaireonTool_PIEWaitFor: short Sleep yields the
	// game thread so the engine tick continues (the editor tick runs on a separate
	// ticker handle but FTSTicker also drains during Sleep yields).
	const double PollIntervalSec = 0.05; // 50ms
	while (FPlatformTime::Seconds() - StartTime < Seconds)
	{
		FPlatformProcess::Sleep(static_cast<float>(FMath::Min(PollIntervalSec, Seconds - (FPlatformTime::Seconds() - StartTime))));
	}

	const double Elapsed = FPlatformTime::Seconds() - StartTime;

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("requested_seconds"), Seconds);
	Data->SetNumberField(TEXT("elapsed_seconds"), Elapsed);

	const FString Summary = FString::Printf(TEXT("Waited %.3fs (requested %.3fs)"), Elapsed, Seconds);
	return MakeSuccessResult(Data, Summary);
}
