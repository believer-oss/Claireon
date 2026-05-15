// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeTool_PaintLayer.h"
#include "Tools/FToolSchemaBuilder.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeEdit.h"
#include "LandscapeLayerInfoObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeTool_PaintLayer::GetName() const
{
	return TEXT("claireon.landscape_paint_layer");
}

FString ClaireonLandscapeTool_PaintLayer::GetDescription() const
{
	return TEXT("Paint a weight layer with a Gaussian falloff brush.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeTool_PaintLayer::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("layer_name"), TEXT("Name of the weight layer to paint."), true);
	Builder.AddObject(TEXT("center"), TEXT("Brush center {x, y} in world space."), true);
	Builder.AddNumber(TEXT("radius"), TEXT("Brush radius (landscape quads)."), true);
	Builder.AddNumber(TEXT("strength"), TEXT("Brush strength (default 1.0)."));
	Builder.AddEnum(TEXT("mode"), TEXT("Paint mode."), { TEXT("paint"), TEXT("erase") });
	return Builder.Build();
}

FToolResult ClaireonLandscapeTool_PaintLayer::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString LayerName;
	if (!Arguments->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: layer_name"));
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

	double Strength = 1.0;
	Arguments->TryGetNumberField(TEXT("strength"), Strength);

	FString ModeStr = TEXT("paint");
	Arguments->TryGetStringField(TEXT("mode"), ModeStr);
	const bool bErase = ModeStr.Equals(TEXT("erase"), ESearchCase::IgnoreCase);

	ULandscapeInfo* LandscapeInfo = Data->LandscapeInfo.Get();
	ALandscapeProxy* Proxy = Data->LandscapeProxy.Get();

	// Find the layer info
	ULandscapeLayerInfoObject* LayerInfoObj = nullptr;
	TArray<FString> AvailableLayerNames;
	for (const FLandscapeInfoLayerSettings& LayerSettings : LandscapeInfo->Layers)
	{
		const FString Name = LayerSettings.GetLayerName().ToString();
		AvailableLayerNames.Add(Name);
		if (LayerSettings.LayerInfoObj && LayerSettings.LayerInfoObj->LayerName == FName(*LayerName))
		{
			LayerInfoObj = LayerSettings.LayerInfoObj;
		}
	}

	if (!LayerInfoObj)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Layer '%s' not found. Available: %s"),
			*LayerName, *FString::Join(AvailableLayerNames, TEXT(", "))));
	}

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
		return MakeErrorResult(TEXT("Paint region is outside landscape bounds"));
	}

	// Read weight data
	TArray<uint8> WeightData;
	WeightData.SetNumUninitialized(DataWidth * DataHeight);

	FLandscapeEditDataInterface EditInterface(LandscapeInfo);
	EditInterface.GetWeightDataFast(LayerInfoObj, X1, Y1, X2, Y2, WeightData.GetData(), 0);

	// Apply Gaussian falloff paint kernel
	const float Sigma = Radius / 3.0f;
	const float SigmaSq2 = 2.0f * Sigma * Sigma;
	const float RadiusSq = Radius * Radius;
	const int32 BrushCenterX = LocalCenterX - X1;
	const int32 BrushCenterY = LocalCenterY - Y1;

	for (int32 Y = 0; Y < DataHeight; ++Y)
	{
		for (int32 X = 0; X < DataWidth; ++X)
		{
			const float DX = static_cast<float>(X - BrushCenterX);
			const float DY = static_cast<float>(Y - BrushCenterY);
			const float DistSq = DX * DX + DY * DY;
			if (DistSq > RadiusSq) continue;

			const float Falloff = FMath::Exp(-DistSq / SigmaSq2);
			const float Blend = Strength * Falloff;
			const int32 Idx = Y * DataWidth + X;
			float CurrentVal = static_cast<float>(WeightData[Idx]);

			if (bErase)
			{
				CurrentVal = FMath::Lerp(CurrentVal, 0.0f, Blend);
			}
			else
			{
				CurrentVal = FMath::Lerp(CurrentVal, 255.0f, Blend);
			}

			WeightData[Idx] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(CurrentVal), 0, 255));
		}
	}

	// Write weight data back
	EditInterface.SetAlphaData(
		LayerInfoObj, X1, Y1, X2, Y2, WeightData.GetData(), 0,
		ELandscapeLayerPaintingRestriction::None, true /*bWeightAdjust*/);

	Data->LastOperationStatus = FString::Printf(
		TEXT("Painted layer '%s' in %dx%d region"), *LayerName, DataWidth, DataHeight);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("paint_layer"));
	ResultData->SetStringField(TEXT("layer_name"), LayerName);
	ResultData->SetNumberField(TEXT("region_width"), DataWidth);
	ResultData->SetNumberField(TEXT("region_height"), DataHeight);
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}
