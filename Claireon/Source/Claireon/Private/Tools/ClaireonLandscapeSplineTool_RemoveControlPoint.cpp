// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeSplineTool_RemoveControlPoint.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeSplineTool_RemoveControlPoint::GetOperation() const { return TEXT("spline_remove_control_point"); }

FString ClaireonLandscapeSplineTool_RemoveControlPoint::GetDescription() const
{
    return TEXT("Remove a spline control point and all segments connected to it. Session-mode tool: open via landscape_spline_open first.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeSplineTool_RemoveControlPoint::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("index"), TEXT("Index of the control point to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonLandscapeSplineTool_RemoveControlPoint::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	ULandscapeSplineControlPoint* PointToRemove = ControlPoints[Index];

	// Collect and remove connected segments
	TArray<ULandscapeSplineSegment*> SegmentsToRemove;
	for (const FLandscapeSplineConnection& Connection : PointToRemove->ConnectedSegments)
	{
		SegmentsToRemove.AddUnique(Connection.Segment);
	}

	auto& Segments = SplinesComp->GetSegments();
	for (ULandscapeSplineSegment* Segment : SegmentsToRemove)
	{
		// Clean up connected segments on the other end
		for (int32 EndIdx = 0; EndIdx < 2; ++EndIdx)
		{
			ULandscapeSplineControlPoint* OtherPoint = Segment->Connections[EndIdx].ControlPoint;
			if (OtherPoint && OtherPoint != PointToRemove)
			{
				OtherPoint->ConnectedSegments.RemoveAll(
					[Segment](const FLandscapeSplineConnection& Conn) { return Conn.Segment == Segment; });
			}
		}
		Segments.Remove(Segment);
	}

	ControlPoints.RemoveAt(Index);

	// Update focused index
	if (Data->FocusedControlPointIndex == Index)
	{
		Data->FocusedControlPointIndex = INDEX_NONE;
	}
	else if (Data->FocusedControlPointIndex > Index)
	{
		--Data->FocusedControlPointIndex;
	}

	SplinesComp->MarkRenderStateDirty();
	Data->LastOperationStatus = FString::Printf(
		TEXT("Removed control point %d and %d connected segment(s)"), Index, SegmentsToRemove.Num());

	return BuildStateResponse(SessionId, Data);
}
