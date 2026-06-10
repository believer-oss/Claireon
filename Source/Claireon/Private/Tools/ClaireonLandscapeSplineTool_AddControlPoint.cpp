// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeSplineTool_AddControlPoint.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplinesComponent.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeSplineTool_AddControlPoint::GetOperation() const { return TEXT("spline_add_control_point"); }

FString ClaireonLandscapeSplineTool_AddControlPoint::GetDescription() const
{
    return TEXT("Create a new spline control point with the given location, rotation, width, and side falloff. Session-mode tool: open via landscape_spline_open first.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeSplineTool_AddControlPoint::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddObject(TEXT("location"), TEXT("World location {x, y, z} for the new control point."), true);
	Builder.AddObject(TEXT("rotation"), TEXT("Optional rotation {pitch, yaw, roll} for the new control point."));
	Builder.AddNumber(TEXT("width"), TEXT("Control point width (default 1000)."));
	Builder.AddNumber(TEXT("side_falloff"), TEXT("Control point side falloff (default 1000)."));
	return Builder.Build();
}

FToolResult ClaireonLandscapeSplineTool_AddControlPoint::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeSplineEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	ULandscapeSplinesComponent* SplinesComp = Data->SplinesComponent.Get();

	// Extract location
	const TSharedPtr<FJsonObject>* LocationObj = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("location"), LocationObj) || !LocationObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: location {x, y, z}"));
	}
	FVector Location = FVector::ZeroVector;
	double Val;
	if ((*LocationObj)->TryGetNumberField(TEXT("x"), Val)) Location.X = Val;
	if ((*LocationObj)->TryGetNumberField(TEXT("y"), Val)) Location.Y = Val;
	if ((*LocationObj)->TryGetNumberField(TEXT("z"), Val)) Location.Z = Val;

	// Optional rotation
	FRotator Rotation = FRotator::ZeroRotator;
	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj)
	{
		if ((*RotObj)->TryGetNumberField(TEXT("pitch"), Val)) Rotation.Pitch = Val;
		if ((*RotObj)->TryGetNumberField(TEXT("yaw"), Val)) Rotation.Yaw = Val;
		if ((*RotObj)->TryGetNumberField(TEXT("roll"), Val)) Rotation.Roll = Val;
	}

	double Width = 1000.0;
	Arguments->TryGetNumberField(TEXT("width"), Width);

	double SideFalloff = 1000.0;
	Arguments->TryGetNumberField(TEXT("side_falloff"), SideFalloff);

	// Create control point
	ULandscapeSplineControlPoint* NewPoint = NewObject<ULandscapeSplineControlPoint>(SplinesComp);
	NewPoint->Location = Location;
	NewPoint->Rotation = Rotation;
	NewPoint->Width = static_cast<float>(Width);
	NewPoint->SideFalloff = static_cast<float>(SideFalloff);

	SplinesComp->GetControlPoints().Add(NewPoint);
	SplinesComp->MarkRenderStateDirty();

	const int32 NewIndex = SplinesComp->GetControlPoints().Num() - 1;
	Data->LastOperationStatus = FString::Printf(TEXT("Added control point at index %d"), NewIndex);

	return BuildStateResponse(SessionId, Data);
}
