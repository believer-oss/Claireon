// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeSplineTool_RemoveSegment.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LandscapeSplineControlPoint.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeSplineTool_RemoveSegment::GetOperation() const { return TEXT("spline_remove_segment"); }

FString ClaireonLandscapeSplineTool_RemoveSegment::GetDescription() const
{
	return TEXT("Remove a spline segment by index, disconnecting it from both control points.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeSplineTool_RemoveSegment::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("index"), TEXT("Index of the segment to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonLandscapeSplineTool_RemoveSegment::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeSplineEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	ULandscapeSplinesComponent* SplinesComp = Data->SplinesComponent.Get();
	auto& Segments = SplinesComp->GetSegments();

	int32 Index = INDEX_NONE;
	if (!Arguments->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("Missing required parameter: index"));
	}

	if (Index < 0 || Index >= Segments.Num())
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Segment index %d out of range [0, %d)"), Index, Segments.Num()));
	}

	ULandscapeSplineSegment* Segment = Segments[Index];

	// Remove from connected control points
	for (int32 EndIdx = 0; EndIdx < 2; ++EndIdx)
	{
		ULandscapeSplineControlPoint* Point = Segment->Connections[EndIdx].ControlPoint;
		if (Point)
		{
			Point->ConnectedSegments.RemoveAll(
				[Segment](const FLandscapeSplineConnection& Conn) { return Conn.Segment == Segment; });
		}
	}

	Segments.RemoveAt(Index);
	SplinesComp->MarkRenderStateDirty();

	Data->LastOperationStatus = FString::Printf(TEXT("Removed segment %d"), Index);

	return BuildStateResponse(SessionId, Data);
}
