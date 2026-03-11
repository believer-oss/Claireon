// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_FlythroughStart.h"
#include "Tools/ClaireonFlythroughManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"

FString ClaireonTool_FlythroughStart::GetName() const
{
	return TEXT("editor.pie.flythrough.start");
}

FString ClaireonTool_FlythroughStart::GetDescription() const
{
	return TEXT("Begin a flythrough with a spline-based flight plan. Activates the debug camera and drives it along waypoints. Returns immediately; poll editor.pie.flythrough.getStatus for progress.");
}

TSharedPtr<FJsonObject> ClaireonTool_FlythroughStart::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("waypoints")));
	Schema->SetArrayField(TEXT("required"), Required);

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// waypoints
	{
		TSharedPtr<FJsonObject> WpArray = MakeShared<FJsonObject>();
		WpArray->SetStringField(TEXT("type"), TEXT("array"));
		WpArray->SetNumberField(TEXT("minItems"), 2);
		WpArray->SetStringField(TEXT("description"),
			TEXT("Array of waypoints. Each must have 'position' {x,y,z}. Optional: 'rotation' {pitch,yaw,roll}, 'lookAt' {x,y,z}, 'speed' (cm/s), 'duration' (s), 'events' [strings], 'pauseDuration' (s)."));

		TSharedPtr<FJsonObject> WpItem = MakeShared<FJsonObject>();
		WpItem->SetStringField(TEXT("type"), TEXT("object"));
		WpArray->SetObjectField(TEXT("items"), WpItem);

		Properties->SetObjectField(TEXT("waypoints"), WpArray);
	}

	// defaultSpeed
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("number"));
		Prop->SetStringField(TEXT("description"), TEXT("Default camera speed in cm/s (default: 1000)"));
		Prop->SetNumberField(TEXT("default"), 1000);
		Properties->SetObjectField(TEXT("defaultSpeed"), Prop);
	}

	// screenshotResolution
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("object"));
		Prop->SetStringField(TEXT("description"), TEXT("Screenshot resolution {x, y}. Default: viewport resolution"));
		Properties->SetObjectField(TEXT("screenshotResolution"), Prop);
	}

	// screenshotDirectory
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Directory for screenshots. Default: Saved/Screenshots/Flythrough/"));
		Properties->SetObjectField(TEXT("screenshotDirectory"), Prop);
	}

	// autoGodMode
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("boolean"));
		Prop->SetStringField(TEXT("description"), TEXT("Enable god mode before flythrough (default: true)"));
		Prop->SetBoolField(TEXT("default"), true);
		Properties->SetObjectField(TEXT("autoGodMode"), Prop);
	}

	// autoScreenshotInterval
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("number"));
		Prop->SetStringField(TEXT("description"), TEXT("Auto-screenshot interval in seconds, 0 = disabled (default: 0)"));
		Prop->SetNumberField(TEXT("default"), 0);
		Properties->SetObjectField(TEXT("autoScreenshotInterval"), Prop);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);
	return Schema;
}

namespace
{
	bool ParseVector(const TSharedPtr<FJsonObject>& Obj, FVector& Out)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		Out.X = Obj->GetNumberField(TEXT("x"));
		Out.Y = Obj->GetNumberField(TEXT("y"));
		Out.Z = Obj->GetNumberField(TEXT("z"));
		return true;
	}

	bool ParseRotator(const TSharedPtr<FJsonObject>& Obj, FRotator& Out)
	{
		if (!Obj.IsValid())
		{
			return false;
		}
		Out.Pitch = Obj->GetNumberField(TEXT("pitch"));
		Out.Yaw = Obj->GetNumberField(TEXT("yaw"));
		Out.Roll = Obj->GetNumberField(TEXT("roll"));
		return true;
	}
}

IClaireonTool::FToolResult ClaireonTool_FlythroughStart::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor || !GEditor->IsPlaySessionInProgress())
	{
		return MakeErrorResult(TEXT("PIE is not running. Start a PIE session first with editor.pie.start"));
	}

	if (FClaireonFlythroughManager::GetActive())
	{
		return MakeErrorResult(TEXT("A flythrough is already active. Stop it first with editor.pie.flythrough.stop"));
	}

	// Parse waypoints
	const TArray<TSharedPtr<FJsonValue>>* WaypointsArray = nullptr;
	if (!Arguments->TryGetArrayField(TEXT("waypoints"), WaypointsArray) || !WaypointsArray || WaypointsArray->Num() < 2)
	{
		return MakeErrorResult(TEXT("'waypoints' array with at least 2 entries is required"));
	}

	TArray<FFlythroughWaypoint> ParsedWaypoints;
	ParsedWaypoints.Reserve(WaypointsArray->Num());

	for (int32 i = 0; i < WaypointsArray->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>* WpObj = nullptr;
		if (!(*WaypointsArray)[i]->TryGetObject(WpObj) || !WpObj || !WpObj->IsValid())
		{
			return MakeErrorResult(FString::Printf(TEXT("waypoints[%d] is not a valid object"), i));
		}

		FFlythroughWaypoint Wp;

		// Position (required)
		const TSharedPtr<FJsonObject>* PosObj = nullptr;
		if (!(*WpObj)->TryGetObjectField(TEXT("position"), PosObj) || !ParseVector(*PosObj, Wp.Position))
		{
			return MakeErrorResult(FString::Printf(TEXT("waypoints[%d].position is required ({x,y,z})"), i));
		}

		// Rotation (optional)
		const TSharedPtr<FJsonObject>* RotObj = nullptr;
		if ((*WpObj)->TryGetObjectField(TEXT("rotation"), RotObj))
		{
			FRotator Rot;
			if (ParseRotator(*RotObj, Rot))
			{
				Wp.Rotation = Rot;
			}
		}

		// LookAt (optional)
		const TSharedPtr<FJsonObject>* LookObj = nullptr;
		if ((*WpObj)->TryGetObjectField(TEXT("lookAt"), LookObj))
		{
			FVector LookAt;
			if (ParseVector(*LookObj, LookAt))
			{
				Wp.LookAt = LookAt;
			}
		}

		// Speed (optional)
		double SpeedVal = 0.0;
		if ((*WpObj)->TryGetNumberField(TEXT("speed"), SpeedVal))
		{
			Wp.Speed = static_cast<float>(SpeedVal);
		}

		// Duration (optional)
		double DurVal = 0.0;
		if ((*WpObj)->TryGetNumberField(TEXT("duration"), DurVal))
		{
			Wp.Duration = static_cast<float>(DurVal);
		}

		// Events (optional)
		const TArray<TSharedPtr<FJsonValue>>* EventsArray = nullptr;
		if ((*WpObj)->TryGetArrayField(TEXT("events"), EventsArray) && EventsArray)
		{
			for (const TSharedPtr<FJsonValue>& Evt : *EventsArray)
			{
				FString EvtStr;
				if (Evt->TryGetString(EvtStr))
				{
					Wp.Events.Add(EvtStr);
				}
			}
		}

		// PauseDuration (optional)
		double PauseVal = 0.0;
		if ((*WpObj)->TryGetNumberField(TEXT("pauseDuration"), PauseVal))
		{
			Wp.PauseDuration = static_cast<float>(PauseVal);
		}

		ParsedWaypoints.Add(MoveTemp(Wp));
	}

	// Parse config
	FFlythroughConfig Config;

	double DefaultSpeedVal = 0.0;
	if (Arguments->TryGetNumberField(TEXT("defaultSpeed"), DefaultSpeedVal))
	{
		Config.DefaultSpeed = static_cast<float>(DefaultSpeedVal);
	}

	const TSharedPtr<FJsonObject>* ResObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("screenshotResolution"), ResObj) && ResObj && ResObj->IsValid())
	{
		Config.ScreenshotResolution.X = static_cast<int32>((*ResObj)->GetNumberField(TEXT("x")));
		Config.ScreenshotResolution.Y = static_cast<int32>((*ResObj)->GetNumberField(TEXT("y")));
	}

	FString SsDir;
	if (Arguments->TryGetStringField(TEXT("screenshotDirectory"), SsDir))
	{
		Config.ScreenshotDirectory = SsDir;
	}

	bool bAutoGod = true;
	if (Arguments->TryGetBoolField(TEXT("autoGodMode"), bAutoGod))
	{
		Config.bAutoGodMode = bAutoGod;
	}

	double AutoSsInterval = 0.0;
	if (Arguments->TryGetNumberField(TEXT("autoScreenshotInterval"), AutoSsInterval))
	{
		Config.AutoScreenshotInterval = static_cast<float>(AutoSsInterval);
	}

	// Create and start the manager
	FClaireonFlythroughManager::DestroyActive();
	FClaireonFlythroughManager::SetActive(MakeUnique<FClaireonFlythroughManager>());

	const int32 WaypointCount = ParsedWaypoints.Num();

	if (!FClaireonFlythroughManager::GetActive()->Start(MoveTemp(ParsedWaypoints), MoveTemp(Config)))
	{
		const FFlythroughStatusInfo Status = FClaireonFlythroughManager::GetActive()->GetStatus();
		const FString Error = Status.ErrorMessage;
		FClaireonFlythroughManager::DestroyActive();
		return MakeErrorResult(FString::Printf(TEXT("Failed to start flythrough: %s"), *Error));
	}

	const float TotalLength = FClaireonFlythroughManager::GetActive()->GetTotalSplineLength();
	const double EstTime = FClaireonFlythroughManager::GetActive()->GetTotalEstimatedTime();

	FString Output = FString::Printf(
		TEXT("status: starting\n")
		TEXT("waypoints: %d\n")
		TEXT("totalSplineLength: %.1f\n")
		TEXT("estimatedDuration: %.1f\n")
		TEXT("message: Flythrough started. Poll editor.pie.flythrough.getStatus for progress."),
		WaypointCount,
		TotalLength,
		EstTime);

	return MakeSuccessResult(nullptr, Output);
}
