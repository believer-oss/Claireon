// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_EditorWaitSeconds.h"
#include "Tools/ClaireonTool_ExecutePython.h"
#include "ClaireonLog.h"

#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

FString ClaireonTool_EditorWaitSeconds::GetCategory() const { return TEXT("editor"); }
FString ClaireonTool_EditorWaitSeconds::GetOperation() const { return TEXT("wait_seconds"); }

FString ClaireonTool_EditorWaitSeconds::GetDescription() const
{
	return TEXT("Wait a wall-clock duration (`seconds`, clamped to [0.0, 300.0]) by blocking the game "
		"thread. Use this instead of `time.sleep`, which is intercepted with a RuntimeWarning. The editor "
		"does NOT tick during the wait; call it as its own direct tool call between other calls, never "
		"from inside a python_execute script, where it stalls everything it might be waiting on. Non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_EditorWaitSeconds::GetInputSchema() const
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

IClaireonTool::FToolResult ClaireonTool_EditorWaitSeconds::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	double Seconds = 0.0;
	if (!Arguments->TryGetNumberField(TEXT("seconds"), Seconds))
	{
		return MakeErrorResult(TEXT("Missing required field: seconds"));
	}
	Seconds = FMath::Clamp(Seconds, 0.0, 300.0);

	// P2-5b: inside python_execute this wait is a pure stall -- Python holds
	// the game thread, so nothing the caller might be waiting on (deferred
	// actions, async loads, PIE conditions) can progress. Warn rather than
	// error for now: the population making this call has been acting on the
	// old description's false promise, and their scripts should keep running
	// while the warning steers them to direct tool calls.
	const bool bInsidePythonExecute = ClaireonTool_ExecutePython::IsPythonExecutionInProgress();

	const double StartTime = FPlatformTime::Seconds();
	// Sleep yields to the OS, not the engine ticker; this loop holds the game thread.
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
	FToolResult Result = MakeSuccessResult(Data, Summary);
	if (bInsidePythonExecute)
	{
		Result.Warnings.Add(TEXT(
			"editor_wait_seconds was called from inside a python_execute script. The editor does not "
			"tick while Python holds the game thread, so this wait stalled the editor for the full "
			"duration and nothing it might have been waiting on made progress. Call it as its own "
			"direct tool call between python_execute calls instead."));
	}
	return Result;
}
