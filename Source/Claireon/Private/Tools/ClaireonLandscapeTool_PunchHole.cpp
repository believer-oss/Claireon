// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeTool_PunchHole.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeEdit.h"
#include "LandscapeLayerInfoObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeTool_PunchHole::GetOperation() const { return TEXT("punch_hole"); }

FString ClaireonLandscapeTool_PunchHole::GetDescription() const
{
    return TEXT("Toggle landscape visibility in a circular region to punch or fill holes. Session-mode tool: open via landscape_open first.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeTool_PunchHole::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddObject(TEXT("center"), TEXT("Region center {x, y} in world space."), true);
	Builder.AddNumber(TEXT("radius"), TEXT("Region radius (landscape quads)."), true);
	Builder.AddBoolean(TEXT("visible"), TEXT("If true, make visible (fill hole); if false, punch hole."));
	return Builder.Build();
}

FToolResult ClaireonLandscapeTool_PunchHole::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	const TSharedPtr<FJsonObject>* CenterObj = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("center"), CenterObj) || !CenterObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: center {x, y}"));
	}
	double CenterWorldX = 0, CenterWorldY = 0;
	(*CenterObj)->TryGetNumberField(TEXT("x"), CenterWorldX);
	(*CenterObj)->TryGetNumberField(TEXT("y"), CenterWorldY);

	double Radius = 0;
	if (!Arguments->TryGetNumberField(TEXT("radius"), Radius) || Radius <= 0)
	{
		return MakeErrorResult(TEXT("Missing or invalid required parameter: radius (must be > 0)"));
	}

	bool bVisible = false;
	Arguments->TryGetBoolField(TEXT("visible"), bVisible);

	ULandscapeLayerInfoObject* VisibilityLayerInfo = ALandscapeProxy::VisibilityLayer;
	if (!VisibilityLayerInfo)
	{
		return MakeErrorResult(TEXT("Landscape visibility layer not available"));
	}

	ALandscapeProxy* Proxy = Data->LandscapeProxy.Get();
	ULandscapeInfo* LandscapeInfo = Data->LandscapeInfo.Get();

	// Convert center and compute bounds
	const FTransform LandscapeTransform = Proxy->GetActorTransform();
	const FVector WorldCenter(CenterWorldX, CenterWorldY, 0.0);
	const FVector LocalCenter = LandscapeTransform.InverseTransformPosition(WorldCenter);

	const int32 LocalCenterX = FMath::RoundToInt(LocalCenter.X);
	const int32 LocalCenterY = FMath::RoundToInt(LocalCenter.Y);
	const int32 IntRadius = FMath::CeilToInt(Radius);

	int32 LandMinX, LandMinY, LandMaxX, LandMaxY;
	LandscapeInfo->GetLandscapeExtent(LandMinX, LandMinY, LandMaxX, LandMaxY);

	const int32 X1 = FMath::Max(LandMinX, LocalCenterX - IntRadius);
	const int32 Y1 = FMath::Max(LandMinY, LocalCenterY - IntRadius);
	const int32 X2 = FMath::Min(LandMaxX, LocalCenterX + IntRadius);
	const int32 Y2 = FMath::Min(LandMaxY, LocalCenterY + IntRadius);

	const int32 DataWidth = X2 - X1 + 1;
	const int32 DataHeight = Y2 - Y1 + 1;

	if (DataWidth <= 0 || DataHeight <= 0)
	{
		return MakeErrorResult(TEXT("Punch hole region is outside landscape bounds"));
	}

	// Fill data buffer: 0 = visible, 255 = hole
	const uint8 FillValue = bVisible ? 0 : 255;
	TArray<uint8> VisibilityData;
	VisibilityData.SetNumUninitialized(DataWidth * DataHeight);

	// Only set pixels within the radius (binary, no falloff)
	const float RadiusSq = Radius * Radius;
	const int32 BrushCenterX = LocalCenterX - X1;
	const int32 BrushCenterY = LocalCenterY - Y1;

	// First, read existing data
	FLandscapeEditDataInterface EditInterface(LandscapeInfo);
	EditInterface.GetWeightDataFast(VisibilityLayerInfo, X1, Y1, X2, Y2, VisibilityData.GetData(), 0);

	// Then modify pixels within radius
	for (int32 Y = 0; Y < DataHeight; ++Y)
	{
		for (int32 X = 0; X < DataWidth; ++X)
		{
			const float DX = static_cast<float>(X - BrushCenterX);
			const float DY = static_cast<float>(Y - BrushCenterY);
			if (DX * DX + DY * DY <= RadiusSq)
			{
				VisibilityData[Y * DataWidth + X] = FillValue;
			}
		}
	}

	// Write
	EditInterface.SetAlphaData(VisibilityLayerInfo, X1, Y1, X2, Y2, VisibilityData.GetData(), 0);

	const FString ActionStr = bVisible ? TEXT("filled hole") : TEXT("punched hole");
	Data->LastOperationStatus = FString::Printf(
		TEXT("Punch hole: %s in %dx%d region"), *ActionStr, DataWidth, DataHeight);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("punch_hole"));
	ResultData->SetBoolField(TEXT("visible"), bVisible);
	ResultData->SetNumberField(TEXT("region_width"), DataWidth);
	ResultData->SetNumberField(TEXT("region_height"), DataHeight);
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}
