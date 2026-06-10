// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeTool_Sculpt.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonLandscapeHelpers.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeEdit.h"
#include "LandscapeDataAccess.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLandscapeTool_Sculpt::GetOperation() const { return TEXT("sculpt"); }

FString ClaireonLandscapeTool_Sculpt::GetDescription() const
{
    return TEXT("Sculpt the landscape heightmap with a circular brush in the open session. Modes: raise, lower, smooth, flatten, erode. Session-mode tool: open via landscape_open first.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeTool_Sculpt::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddObject(TEXT("center"), TEXT("Brush center {x, y} in world space."), true);
	Builder.AddNumber(TEXT("radius"), TEXT("Brush radius (landscape quads)."), true);
	Builder.AddNumber(TEXT("strength"), TEXT("Brush strength (default 1.0)."));
	Builder.AddEnum(TEXT("mode"), TEXT("Brush mode."), { TEXT("raise"), TEXT("lower"), TEXT("smooth"), TEXT("flatten"), TEXT("erode") });
	Builder.AddNumber(TEXT("target_height"), TEXT("Target height (flatten mode only)."));
	return Builder.Build();
}

FToolResult ClaireonLandscapeTool_Sculpt::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FLandscapeEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	// Extract parameters
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

	FString ModeStr = TEXT("raise");
	Arguments->TryGetStringField(TEXT("mode"), ModeStr);

	double TargetHeight = 0.0;
	Arguments->TryGetNumberField(TEXT("target_height"), TargetHeight);

	// Map mode string to enum
	EClaireonBrushMode BrushMode = EClaireonBrushMode::Raise;
	if (ModeStr.Equals(TEXT("lower"), ESearchCase::IgnoreCase)) BrushMode = EClaireonBrushMode::Lower;
	else if (ModeStr.Equals(TEXT("smooth"), ESearchCase::IgnoreCase)) BrushMode = EClaireonBrushMode::Smooth;
	else if (ModeStr.Equals(TEXT("flatten"), ESearchCase::IgnoreCase)) BrushMode = EClaireonBrushMode::Flatten;
	else if (ModeStr.Equals(TEXT("erode"), ESearchCase::IgnoreCase)) BrushMode = EClaireonBrushMode::Erode;

	ALandscapeProxy* Proxy = Data->LandscapeProxy.Get();
	ULandscapeInfo* LandscapeInfo = Data->LandscapeInfo.Get();

	// Convert world-space center to landscape-local coordinates
	const FTransform LandscapeTransform = Proxy->GetActorTransform();
	const FVector WorldCenter(CenterWorldX, CenterWorldY, 0.0);
	const FVector LocalCenter = LandscapeTransform.InverseTransformPosition(WorldCenter);

	const int32 LocalCenterX = FMath::RoundToInt(LocalCenter.X);
	const int32 LocalCenterY = FMath::RoundToInt(LocalCenter.Y);
	const int32 IntRadius = FMath::CeilToInt(Radius);

	// Get landscape extents
	int32 LandMinX = 0, LandMinY = 0, LandMaxX = 0, LandMaxY = 0;
	LandscapeInfo->GetLandscapeExtent(LandMinX, LandMinY, LandMaxX, LandMaxY);

	// Compute bounding box clamped to landscape
	const int32 X1 = FMath::Max(LandMinX, LocalCenterX - IntRadius);
	const int32 Y1 = FMath::Max(LandMinY, LocalCenterY - IntRadius);
	const int32 X2 = FMath::Min(LandMaxX, LocalCenterX + IntRadius);
	const int32 Y2 = FMath::Min(LandMaxY, LocalCenterY + IntRadius);

	const int32 DataWidth = X2 - X1 + 1;
	const int32 DataHeight = Y2 - Y1 + 1;

	if (DataWidth <= 0 || DataHeight <= 0)
	{
		return MakeErrorResult(TEXT("Sculpt region is outside landscape bounds"));
	}

	// Safety check
	if (DataWidth > 4096 || DataHeight > 4096)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Sculpt region too large (%dx%d). Max 4096x4096."), DataWidth, DataHeight));
	}

	// Check for unloaded components
	if (LandscapeInfo->HasUnloadedComponentsInRegion(X1, Y1, X2, Y2))
	{
		return MakeErrorResult(TEXT("Region contains unloaded landscape components. Load the area first."));
	}

	// Read height data
	TArray<uint16> HeightData;
	HeightData.SetNumUninitialized(DataWidth * DataHeight);

	FLandscapeEditDataInterface EditInterface(LandscapeInfo);
	EditInterface.GetHeightDataFast(X1, Y1, X2, Y2, HeightData.GetData(), 0);

	// Apply brush
	const float TargetHeightUint16 = (BrushMode == EClaireonBrushMode::Flatten)
		? static_cast<float>(ClaireonLandscapeHelpers::HeightWorldToUint16(TargetHeight))
		: 0.0f;

	const int32 BrushCenterX = LocalCenterX - X1;
	const int32 BrushCenterY = LocalCenterY - Y1;

	ClaireonLandscapeHelpers::ApplyBrushKernel(
		HeightData, DataWidth, DataHeight,
		BrushCenterX, BrushCenterY,
		Radius, Strength, BrushMode, TargetHeightUint16);

	// Write height data back
	EditInterface.SetHeightData(X1, Y1, X2, Y2, HeightData.GetData(), 0, true /*CalcNormals*/);

	Data->LastOperationStatus = FString::Printf(
		TEXT("Sculpted %dx%d region at (%d,%d) mode=%s"), DataWidth, DataHeight, LocalCenterX, LocalCenterY, *ModeStr);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("sculpt"));
	ResultData->SetStringField(TEXT("mode"), ModeStr);
	ResultData->SetNumberField(TEXT("region_width"), DataWidth);
	ResultData->SetNumberField(TEXT("region_height"), DataHeight);
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}
