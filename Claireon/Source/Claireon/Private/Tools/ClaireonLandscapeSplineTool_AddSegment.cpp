// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeSplineTool_AddSegment.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeSplineTool_AddSegment::GetOperation() const { return TEXT("spline_add_segment"); }

FString ClaireonLandscapeSplineTool_AddSegment::GetDescription() const
{
	return TEXT("Connect two existing control points with a new spline segment.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeSplineTool_AddSegment::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("start_index"), TEXT("Index of the start control point."), true);
	Builder.AddInteger(TEXT("end_index"), TEXT("Index of the end control point."), true);
	return Builder.Build();
}

FToolResult ClaireonLandscapeSplineTool_AddSegment::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeSplineEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	ULandscapeSplinesComponent* SplinesComp = Data->SplinesComponent.Get();
	auto& ControlPoints = SplinesComp->GetControlPoints();

	int32 StartIndex = INDEX_NONE, EndIndex = INDEX_NONE;
	if (!Arguments->TryGetNumberField(TEXT("start_index"), StartIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: start_index"));
	}
	if (!Arguments->TryGetNumberField(TEXT("end_index"), EndIndex))
	{
		return MakeErrorResult(TEXT("Missing required parameter: end_index"));
	}

	if (StartIndex < 0 || StartIndex >= ControlPoints.Num())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Start control point index %d out of range [0, %d)"), StartIndex, ControlPoints.Num()));
	}
	if (EndIndex < 0 || EndIndex >= ControlPoints.Num())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("End control point index %d out of range [0, %d)"), EndIndex, ControlPoints.Num()));
	}
	if (StartIndex == EndIndex)
	{
		return MakeErrorResult(TEXT("Cannot connect a control point to itself"));
	}

	ULandscapeSplineControlPoint* StartPoint = ControlPoints[StartIndex];
	ULandscapeSplineControlPoint* EndPoint = ControlPoints[EndIndex];

	// Create segment
	ULandscapeSplineSegment* NewSegment = NewObject<ULandscapeSplineSegment>(SplinesComp);
	NewSegment->Connections[0].ControlPoint = StartPoint;
	NewSegment->Connections[1].ControlPoint = EndPoint;

	// Wire up both control points
	FLandscapeSplineConnection StartConn;
	StartConn.Segment = NewSegment;
	StartConn.End = 0;
	StartPoint->ConnectedSegments.Add(StartConn);

	FLandscapeSplineConnection EndConn;
	EndConn.Segment = NewSegment;
	EndConn.End = 1;
	EndPoint->ConnectedSegments.Add(EndConn);

	// Compute tangent data
	NewSegment->UpdateSplinePoints();

	SplinesComp->GetSegments().Add(NewSegment);
	SplinesComp->MarkRenderStateDirty();

	Data->LastOperationStatus = FString::Printf(
		TEXT("Added segment connecting control points %d and %d"), StartIndex, EndIndex);

	return BuildStateResponse(SessionId, Data);
}
