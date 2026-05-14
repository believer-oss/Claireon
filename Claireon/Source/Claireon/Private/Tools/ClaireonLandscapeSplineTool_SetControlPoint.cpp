// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeSplineTool_SetControlPoint.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplinesComponent.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeSplineTool_SetControlPoint::GetOperation() const { return TEXT("spline_set_control_point"); }

FString ClaireonLandscapeSplineTool_SetControlPoint::GetDescription() const
{
	return TEXT("Modify properties of an existing spline control point (location, rotation, width, side falloff).");
}

TSharedPtr<FJsonObject> ClaireonLandscapeSplineTool_SetControlPoint::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("index"), TEXT("Index of the control point to modify."), true);
	Builder.AddObject(TEXT("location"), TEXT("Optional new location {x, y, z}."));
	Builder.AddObject(TEXT("rotation"), TEXT("Optional new rotation {pitch, yaw, roll}."));
	Builder.AddNumber(TEXT("width"), TEXT("Optional new control point width."));
	Builder.AddNumber(TEXT("side_falloff"), TEXT("Optional new control point side falloff."));
	return Builder.Build();
}

FToolResult ClaireonLandscapeSplineTool_SetControlPoint::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeSplineEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	ULandscapeSplinesComponent* SplinesComp = Data->SplinesComponent.Get();
	const auto& ControlPoints = SplinesComp->GetControlPoints();

	int32 Index = INDEX_NONE;
	if (!Arguments->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("Missing required parameter: index"));
	}

	if (Index < 0 || Index >= ControlPoints.Num())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Control point index %d out of range [0, %d)"), Index, ControlPoints.Num()));
	}

	ULandscapeSplineControlPoint* Point = ControlPoints[Index];
	bool bLocationOrRotationChanged = false;
	double Val;

	// Apply optional properties
	const TSharedPtr<FJsonObject>* LocationObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("location"), LocationObj) && LocationObj)
	{
		if ((*LocationObj)->TryGetNumberField(TEXT("x"), Val)) { Point->Location.X = Val; bLocationOrRotationChanged = true; }
		if ((*LocationObj)->TryGetNumberField(TEXT("y"), Val)) { Point->Location.Y = Val; bLocationOrRotationChanged = true; }
		if ((*LocationObj)->TryGetNumberField(TEXT("z"), Val)) { Point->Location.Z = Val; bLocationOrRotationChanged = true; }
	}

	const TSharedPtr<FJsonObject>* RotObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("rotation"), RotObj) && RotObj)
	{
		if ((*RotObj)->TryGetNumberField(TEXT("pitch"), Val)) { Point->Rotation.Pitch = Val; bLocationOrRotationChanged = true; }
		if ((*RotObj)->TryGetNumberField(TEXT("yaw"), Val)) { Point->Rotation.Yaw = Val; bLocationOrRotationChanged = true; }
		if ((*RotObj)->TryGetNumberField(TEXT("roll"), Val)) { Point->Rotation.Roll = Val; bLocationOrRotationChanged = true; }
	}

	if (Arguments->TryGetNumberField(TEXT("width"), Val)) Point->Width = static_cast<float>(Val);
	if (Arguments->TryGetNumberField(TEXT("side_falloff"), Val)) Point->SideFalloff = static_cast<float>(Val);

	if (bLocationOrRotationChanged)
	{
		Point->UpdateSplinePoints(true /*bUpdateCollision*/, true /*bUpdateAttachedSegments*/);
	}

	SplinesComp->MarkRenderStateDirty();
	Data->LastOperationStatus = FString::Printf(TEXT("Updated control point %d"), Index);

	return BuildStateResponse(SessionId, Data);
}
