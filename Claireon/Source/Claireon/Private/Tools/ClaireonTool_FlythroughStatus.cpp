// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_FlythroughStatus.h"
#include "Tools/ClaireonFlythroughManager.h"

FString ClaireonTool_FlythroughStatus::GetName() const
{
	return TEXT("editor.pie.flythrough.getStatus");
}

FString ClaireonTool_FlythroughStatus::GetDescription() const
{
	return TEXT("Poll flythrough progress, current position, elapsed time, and state.");
}

TSharedPtr<FJsonObject> ClaireonTool_FlythroughStatus::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	return Schema;
}

namespace
{
	FString StateToString(EFlythroughState InState)
	{
		switch (InState)
		{
		case EFlythroughState::Idle:            return TEXT("Idle");
		case EFlythroughState::Initializing:    return TEXT("Initializing");
		case EFlythroughState::Flying:          return TEXT("Flying");
		case EFlythroughState::Paused:          return TEXT("Paused");
		case EFlythroughState::WaitingForEvent: return TEXT("WaitingForEvent");
		case EFlythroughState::Complete:        return TEXT("Complete");
		case EFlythroughState::Error:           return TEXT("Error");
		default:                                return TEXT("Unknown");
		}
	}
}

IClaireonTool::FToolResult ClaireonTool_FlythroughStatus::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FClaireonFlythroughManager* Manager = FClaireonFlythroughManager::GetActive();
	if (!Manager)
	{
		return MakeSuccessResult(nullptr, TEXT("state: Idle\nmessage: No flythrough is active"));
	}

	const FFlythroughStatusInfo Status = Manager->GetStatus();
	const FString StateStr = StateToString(Status.State);

	FString Output = FString::Printf(
		TEXT("state: %s\n")
		TEXT("progress: %.3f\n")
		TEXT("elapsedTime: %.1f\n")
		TEXT("totalEstimatedTime: %.1f\n")
		TEXT("currentWaypointIndex: %d\n")
		TEXT("totalWaypoints: %d\n")
		TEXT("position: X=%.2f Y=%.2f Z=%.2f\n")
		TEXT("rotation: Pitch=%.2f Yaw=%.2f Roll=%.2f\n")
		TEXT("screenshotsTaken: %d\n")
		TEXT("traceActive: %s\n")
		TEXT("screenshotDirectory: %s"),
		*StateStr,
		Status.Progress,
		Status.ElapsedTime,
		Status.TotalEstimatedTime,
		Status.CurrentWaypointIndex,
		Status.TotalWaypoints,
		Status.Position.X, Status.Position.Y, Status.Position.Z,
		Status.Rotation.Pitch, Status.Rotation.Yaw, Status.Rotation.Roll,
		Status.ScreenshotsTaken,
		Status.bTraceActive ? TEXT("true") : TEXT("false"),
		*Status.ScreenshotDirectory);

	if (Status.State == EFlythroughState::Complete)
	{
		Output += FString::Printf(TEXT("\ntotalScreenshots: %d"), Status.ScreenshotsTaken);
		if (!Status.TraceFilePath.IsEmpty())
		{
			Output += FString::Printf(TEXT("\ntraceFilePath: %s"), *Status.TraceFilePath);
		}
	}

	if (Status.State == EFlythroughState::Error)
	{
		Output += FString::Printf(TEXT("\nerror: %s"), *Status.ErrorMessage);
	}

	return MakeSuccessResult(nullptr, Output);
}
