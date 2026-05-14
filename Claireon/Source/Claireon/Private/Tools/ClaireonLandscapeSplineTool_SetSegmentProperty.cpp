// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeSplineTool_SetSegmentProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LandscapeSplineSegment.h"
#include "LandscapeSplinesComponent.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeSplineTool_SetSegmentProperty::GetOperation() const { return TEXT("spline_set_segment_property"); }

FString ClaireonLandscapeSplineTool_SetSegmentProperty::GetDescription() const
{
	return TEXT("Modify spline segment properties (layer_name, raise_terrain, lower_terrain).");
}

TSharedPtr<FJsonObject> ClaireonLandscapeSplineTool_SetSegmentProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("index"), TEXT("Index of the segment to modify."), true);
	Builder.AddString(TEXT("layer_name"), TEXT("Optional paint layer name used when applying to the landscape."));
	Builder.AddBoolean(TEXT("raise_terrain"), TEXT("When true, segment raises the terrain along its path."));
	Builder.AddBoolean(TEXT("lower_terrain"), TEXT("When true, segment lowers the terrain along its path."));
	return Builder.Build();
}

FToolResult ClaireonLandscapeSplineTool_SetSegmentProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeSplineEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	ULandscapeSplinesComponent* SplinesComp = Data->SplinesComponent.Get();
	const auto& Segments = SplinesComp->GetSegments();

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

	FString LayerName;
	if (Arguments->TryGetStringField(TEXT("layer_name"), LayerName))
	{
		Segment->LayerName = FName(*LayerName);
	}

	bool bVal;
	if (Arguments->TryGetBoolField(TEXT("raise_terrain"), bVal))
	{
		Segment->bRaiseTerrain = bVal;
	}
	if (Arguments->TryGetBoolField(TEXT("lower_terrain"), bVal))
	{
		Segment->bLowerTerrain = bVal;
	}

	SplinesComp->MarkRenderStateDirty();
	Data->LastOperationStatus = FString::Printf(TEXT("Updated segment %d properties"), Index);

	return BuildStateResponse(SessionId, Data);
}
