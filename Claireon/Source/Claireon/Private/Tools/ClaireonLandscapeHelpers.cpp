// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeHelpers.h"
#include "ClaireonLog.h"
#include "LandscapeInfoMap.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"
#include "LandscapeSplinesComponent.h"
#include "LandscapeDataAccess.h"
#include "InstancedFoliageActor.h"
#include "Dom/JsonValue.h"
#include "Materials/MaterialInterface.h"

namespace ClaireonLandscapeHelpers
{

TArray<TPair<ULandscapeInfo*, ALandscapeProxy*>> FindLandscapeInWorld(UWorld* World, const FString& NameFilter)
{
	TArray<TPair<ULandscapeInfo*, ALandscapeProxy*>> Result;

	if (!World)
	{
		return Result;
	}

	ULandscapeInfoMap* InfoMap = ULandscapeInfoMap::FindLandscapeInfoMap(World);
	if (!InfoMap)
	{
		return Result;
	}

	for (auto& Pair : InfoMap->Map)
	{
		ULandscapeInfo* LandscapeInfo = Pair.Value;
		if (!LandscapeInfo)
		{
			continue;
		}

		ALandscapeProxy* Proxy = LandscapeInfo->GetLandscapeProxy();
		if (!Proxy)
		{
			UE_LOG(LogClaireon, Warning, TEXT("FindLandscapeInWorld: ULandscapeInfo entry has null proxy (GUID: %s)"), *Pair.Key.ToString());
			continue;
		}

		if (!NameFilter.IsEmpty())
		{
			const FString ActorLabel = Proxy->GetActorLabel();
			if (!ActorLabel.Contains(NameFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		Result.Emplace(LandscapeInfo, Proxy);
	}

	return Result;
}

AInstancedFoliageActor* GetOrCreateFoliageActor(UWorld* World, FString& OutError)
{
	if (!World)
	{
		OutError = TEXT("World is null");
		return nullptr;
	}

#if WITH_EDITOR
	AInstancedFoliageActor* IFA = AInstancedFoliageActor::Get(World, true);
	if (!IFA)
	{
		OutError = TEXT("Failed to create AInstancedFoliageActor");
		return nullptr;
	}
	return IFA;
#else
	OutError = TEXT("Foliage editing requires WITH_EDITOR");
	return nullptr;
#endif
}

TSharedPtr<FJsonObject> BuildLandscapeInfoJson(ULandscapeInfo* LandscapeInfo, const FString& DetailLevel)
{
	if (!LandscapeInfo)
	{
		return nullptr;
	}

	ALandscapeProxy* Proxy = LandscapeInfo->GetLandscapeProxy();
	if (!Proxy)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();

	// Always-present fields
	Result->SetStringField(TEXT("name"), Proxy->GetActorLabel());
	Result->SetStringField(TEXT("actor_path"), Proxy->GetPathName());

	// Dimensions
	int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	const bool bExtentValid = LandscapeInfo->GetLandscapeExtent(MinX, MinY, MaxX, MaxY);

	TSharedPtr<FJsonObject> Dimensions = MakeShared<FJsonObject>();
	Dimensions->SetNumberField(TEXT("min_x"), MinX);
	Dimensions->SetNumberField(TEXT("min_y"), MinY);
	Dimensions->SetNumberField(TEXT("max_x"), MaxX);
	Dimensions->SetNumberField(TEXT("max_y"), MaxY);
	Result->SetObjectField(TEXT("dimensions"), Dimensions);

	if (!bExtentValid)
	{
		Result->SetBoolField(TEXT("dimensions_valid"), false);
	}

	// Component count
	Result->SetNumberField(TEXT("component_count"), LandscapeInfo->XYtoComponentMap.Num());

	// Transform
	const FTransform ActorTransform = Proxy->GetActorTransform();
	const FVector Location = ActorTransform.GetLocation();
	const FVector Scale = ActorTransform.GetScale3D();

	TSharedPtr<FJsonObject> Transform = MakeShared<FJsonObject>();
	{
		TArray<TSharedPtr<FJsonValue>> LocationArray;
		LocationArray.Add(MakeShared<FJsonValueNumber>(Location.X));
		LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Y));
		LocationArray.Add(MakeShared<FJsonValueNumber>(Location.Z));
		Transform->SetArrayField(TEXT("location"), LocationArray);

		TArray<TSharedPtr<FJsonValue>> ScaleArray;
		ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.X));
		ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Y));
		ScaleArray.Add(MakeShared<FJsonValueNumber>(Scale.Z));
		Transform->SetArrayField(TEXT("scale"), ScaleArray);
	}
	Result->SetObjectField(TEXT("transform"), Transform);

	// Static lighting LOD
	Result->SetNumberField(TEXT("static_lighting_lod"), Proxy->StaticLightingLOD);

	// Full detail: material, weight layers, splines
	if (DetailLevel.Equals(TEXT("full"), ESearchCase::IgnoreCase) || DetailLevel.IsEmpty())
	{
		// Material
		if (Proxy->LandscapeMaterial)
		{
			Result->SetStringField(TEXT("material"), Proxy->LandscapeMaterial->GetPathName());
		}
		else
		{
			Result->SetField(TEXT("material"), MakeShared<FJsonValueNull>());
		}

		// Weight layers
		TArray<TSharedPtr<FJsonValue>> LayerArray;
		for (const FLandscapeInfoLayerSettings& LayerSettings : LandscapeInfo->Layers)
		{
			LayerArray.Add(MakeShared<FJsonValueString>(LayerSettings.GetLayerName().ToString()));
		}
		Result->SetArrayField(TEXT("weight_layers"), LayerArray);

		// Splines
		ULandscapeSplinesComponent* SplinesComponent = Proxy->GetSplinesComponent();
		TSharedPtr<FJsonObject> Splines = MakeShared<FJsonObject>();
		if (SplinesComponent)
		{
			Splines->SetNumberField(TEXT("control_point_count"), SplinesComponent->GetControlPoints().Num());
			Splines->SetNumberField(TEXT("segment_count"), SplinesComponent->GetSegments().Num());
		}
		else
		{
			Splines->SetNumberField(TEXT("control_point_count"), 0);
			Splines->SetNumberField(TEXT("segment_count"), 0);
		}
		Result->SetObjectField(TEXT("splines"), Splines);
	}

	return Result;
}

void ApplyBrushKernel(
	TArray<uint16>& HeightData,
	int32 DataWidth,
	int32 DataHeight,
	int32 CenterX,
	int32 CenterY,
	float Radius,
	float Strength,
	EClaireonBrushMode Mode,
	float TargetHeight)
{
	if (Radius <= 0.0f || Strength <= 0.0f)
	{
		return;
	}

	const float Sigma = Radius / 3.0f;
	const float SigmaSq2 = 2.0f * Sigma * Sigma;
	const int32 IntRadius = FMath::CeilToInt(Radius);

	const int32 MinX = FMath::Max(0, CenterX - IntRadius);
	const int32 MaxX = FMath::Min(DataWidth - 1, CenterX + IntRadius);
	const int32 MinY = FMath::Max(0, CenterY - IntRadius);
	const int32 MaxY = FMath::Min(DataHeight - 1, CenterY + IntRadius);

	const float RadiusSq = Radius * Radius;

	switch (Mode)
	{
	case EClaireonBrushMode::Raise:
	case EClaireonBrushMode::Lower:
	{
		const float Sign = (Mode == EClaireonBrushMode::Raise) ? 1.0f : -1.0f;
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const float DX = static_cast<float>(X - CenterX);
				const float DY = static_cast<float>(Y - CenterY);
				const float DistSq = DX * DX + DY * DY;
				if (DistSq > RadiusSq) continue;

				const float Falloff = FMath::Exp(-DistSq / SigmaSq2);
				const float Delta = Sign * Strength * Falloff * 256.0f;
				const int32 Idx = Y * DataWidth + X;
				const float NewVal = static_cast<float>(HeightData[Idx]) + Delta;
				HeightData[Idx] = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(NewVal), 0, 65535));
			}
		}
		break;
	}

	case EClaireonBrushMode::Smooth:
	{
		// Work on a copy to avoid reading already-smoothed values
		TArray<uint16> Original = HeightData;
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const float DX = static_cast<float>(X - CenterX);
				const float DY = static_cast<float>(Y - CenterY);
				const float DistSq = DX * DX + DY * DY;
				if (DistSq > RadiusSq) continue;

				const float Falloff = FMath::Exp(-DistSq / SigmaSq2);

				// 3x3 box average
				float Sum = 0.0f;
				int32 Count = 0;
				for (int32 NY = FMath::Max(0, Y - 1); NY <= FMath::Min(DataHeight - 1, Y + 1); ++NY)
				{
					for (int32 NX = FMath::Max(0, X - 1); NX <= FMath::Min(DataWidth - 1, X + 1); ++NX)
					{
						Sum += static_cast<float>(Original[NY * DataWidth + NX]);
						++Count;
					}
				}
				const float Avg = Sum / static_cast<float>(Count);
				const int32 Idx = Y * DataWidth + X;
				const float Blend = Strength * Falloff;
				const float NewVal = FMath::Lerp(static_cast<float>(Original[Idx]), Avg, Blend);
				HeightData[Idx] = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(NewVal), 0, 65535));
			}
		}
		break;
	}

	case EClaireonBrushMode::Flatten:
	{
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const float DX = static_cast<float>(X - CenterX);
				const float DY = static_cast<float>(Y - CenterY);
				const float DistSq = DX * DX + DY * DY;
				if (DistSq > RadiusSq) continue;

				const float Falloff = FMath::Exp(-DistSq / SigmaSq2);
				const int32 Idx = Y * DataWidth + X;
				const float Blend = Strength * Falloff;
				const float NewVal = FMath::Lerp(static_cast<float>(HeightData[Idx]), TargetHeight, Blend);
				HeightData[Idx] = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(NewVal), 0, 65535));
			}
		}
		break;
	}

	case EClaireonBrushMode::Erode:
	{
		// Single-pass thermal erosion
		// Talus threshold scales inversely with Strength (higher strength = more erosion)
		const float TalusThreshold = 256.0f / FMath::Max(Strength, 0.01f);
		TArray<uint16> Original = HeightData;
		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const float DX = static_cast<float>(X - CenterX);
				const float DY = static_cast<float>(Y - CenterY);
				const float DistSq = DX * DX + DY * DY;
				if (DistSq > RadiusSq) continue;

				const int32 Idx = Y * DataWidth + X;
				const float CurHeight = static_cast<float>(Original[Idx]);

				// Check 4-connected neighbors
				float MaxDiff = 0.0f;
				int32 LowestNeighborIdx = INDEX_NONE;
				const int32 Neighbors[4][2] = { {X-1,Y}, {X+1,Y}, {X,Y-1}, {X,Y+1} };
				for (int32 N = 0; N < 4; ++N)
				{
					const int32 NX = Neighbors[N][0];
					const int32 NY = Neighbors[N][1];
					if (NX < 0 || NX >= DataWidth || NY < 0 || NY >= DataHeight) continue;
					const int32 NIdx = NY * DataWidth + NX;
					const float Diff = CurHeight - static_cast<float>(Original[NIdx]);
					if (Diff > MaxDiff)
					{
						MaxDiff = Diff;
						LowestNeighborIdx = NIdx;
					}
				}

				if (MaxDiff > TalusThreshold && LowestNeighborIdx != INDEX_NONE)
				{
					const float Transfer = (MaxDiff - TalusThreshold) * 0.5f;
					const float NewCur = CurHeight - Transfer;
					const float NewNeighbor = static_cast<float>(HeightData[LowestNeighborIdx]) + Transfer;
					HeightData[Idx] = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(NewCur), 0, 65535));
					HeightData[LowestNeighborIdx] = static_cast<uint16>(FMath::Clamp(FMath::RoundToInt(NewNeighbor), 0, 65535));
				}
			}
		}
		break;
	}
	}
}

uint16 HeightWorldToUint16(float WorldHeight)
{
	return static_cast<uint16>(FMath::RoundToInt(
		FMath::Clamp<float>(WorldHeight * LANDSCAPE_INV_ZSCALE + 32768.0f, 0.f, 65535.f)));
}

float HeightUint16ToWorld(uint16 RawHeight)
{
	return (static_cast<float>(RawHeight) - 32768.0f) * LANDSCAPE_ZSCALE;
}

} // namespace ClaireonLandscapeHelpers
