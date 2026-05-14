// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonFoliageTool_Scatter.h"
#include "Tools/FToolSchemaBuilder.h"
#include "InstancedFoliageActor.h"
#include "InstancedFoliage.h"
#include "FoliageType.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonFoliageTool_Scatter::GetOperation() const { return TEXT("scatter"); }

FString ClaireonFoliageTool_Scatter::GetDescription() const
{
	return TEXT("Procedurally scatter foliage within a rectangular area using a jittered grid.");
}

TSharedPtr<FJsonObject> ClaireonFoliageTool_Scatter::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("foliage_type"), TEXT("Name or asset path of the registered foliage type."), true);
	Builder.AddObject(TEXT("bounds_min"), TEXT("Bounds min corner {x, y}."), true);
	Builder.AddObject(TEXT("bounds_max"), TEXT("Bounds max corner {x, y}."), true);
	Builder.AddNumber(TEXT("spacing"), TEXT("Grid spacing (uu). Instance count is clamped to 100000."), true);
	Builder.AddObject(TEXT("scale_range"), TEXT("{min, max} uniform scale range (default 1.0)."));
	Builder.AddBoolean(TEXT("align_to_normal"), TEXT("Align instances to ground normal (default true)."));
	return Builder.Build();
}

FToolResult ClaireonFoliageTool_Scatter::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if WITH_EDITOR
	FString SessionId;
	FFoliageEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	FString TypeName;
	if (!Arguments->TryGetStringField(TEXT("foliage_type"), TypeName) || TypeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: foliage_type"));
	}

	const TSharedPtr<FJsonObject>* MinObj = nullptr;
	const TSharedPtr<FJsonObject>* MaxObj = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("bounds_min"), MinObj) || !MinObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: bounds_min {x, y}"));
	}
	if (!Arguments->TryGetObjectField(TEXT("bounds_max"), MaxObj) || !MaxObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: bounds_max {x, y}"));
	}

	double MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	(*MinObj)->TryGetNumberField(TEXT("x"), MinX);
	(*MinObj)->TryGetNumberField(TEXT("y"), MinY);
	(*MaxObj)->TryGetNumberField(TEXT("x"), MaxX);
	(*MaxObj)->TryGetNumberField(TEXT("y"), MaxY);

	double Spacing = 0;
	if (!Arguments->TryGetNumberField(TEXT("spacing"), Spacing) || Spacing <= 0)
	{
		return MakeErrorResult(TEXT("Missing or invalid required parameter: spacing"));
	}

	double ScaleMin = 1.0, ScaleMax = 1.0;
	const TSharedPtr<FJsonObject>* ScaleRangeObj = nullptr;
	if (Arguments->TryGetObjectField(TEXT("scale_range"), ScaleRangeObj) && ScaleRangeObj)
	{
		(*ScaleRangeObj)->TryGetNumberField(TEXT("min"), ScaleMin);
		(*ScaleRangeObj)->TryGetNumberField(TEXT("max"), ScaleMax);
	}

	bool bAlignToNormal = true;
	Arguments->TryGetBoolField(TEXT("align_to_normal"), bAlignToNormal);

	// Estimate instance count
	const double AreaX = FMath::Abs(MaxX - MinX);
	const double AreaY = FMath::Abs(MaxY - MinY);
	const int32 GridCountX = FMath::Max(1, FMath::CeilToInt(AreaX / Spacing));
	const int32 GridCountY = FMath::Max(1, FMath::CeilToInt(AreaY / Spacing));
	const int32 EstimatedCount = GridCountX * GridCountY;

	if (EstimatedCount > 100000)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Estimated instance count %d exceeds limit of 100000. Increase spacing or reduce area."),
			EstimatedCount));
	}

	AInstancedFoliageActor* IFA = Data->FoliageActor.Get();
	UFoliageType* FoliageType = FindFoliageTypeInActor(IFA, TypeName);
	if (!FoliageType)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Foliage type '%s' not found. Call add_foliage_type first."), *TypeName));
	}

	FFoliageInfo* FoliageInfo = IFA->FindInfo(FoliageType);
	if (!FoliageInfo)
	{
		return MakeErrorResult(TEXT("Internal error: foliage info not found for type"));
	}

	UWorld* World = IFA->GetWorld();
	int32 Placed = 0;
	int32 Skipped = 0;

	TArray<const FFoliageInstance*> InstancesToAdd;
	TArray<FFoliageInstance> InstanceStorage;
	InstanceStorage.Reserve(EstimatedCount);

	const float HalfJitter = Spacing * 0.5f;

	for (int32 GY = 0; GY < GridCountY; ++GY)
	{
		for (int32 GX = 0; GX < GridCountX; ++GX)
		{
			// Grid center + jitter
			const float PosX = MinX + (GX + 0.5f) * Spacing + FMath::FRandRange(-HalfJitter, HalfJitter);
			const float PosY = MinY + (GY + 0.5f) * Spacing + FMath::FRandRange(-HalfJitter, HalfJitter);

			// Ray trace for ground
			FHitResult HitResult;
			const FVector TraceStart(PosX, PosY, 100000.0);
			const FVector TraceEnd(PosX, PosY, -100000.0);
			FCollisionQueryParams QueryParams;
			QueryParams.bTraceComplex = true;

			if (!World->LineTraceSingleByChannel(HitResult, TraceStart, TraceEnd, ECC_WorldStatic, QueryParams))
			{
				++Skipped;
				continue;
			}

			FFoliageInstance& NewInstance = InstanceStorage.AddDefaulted_GetRef();
			NewInstance.Location = HitResult.ImpactPoint;

			const float Scale = FMath::FRandRange(ScaleMin, ScaleMax);
			NewInstance.DrawScale3D = FVector3f(Scale, Scale, Scale);
			NewInstance.Rotation = FRotator(0.0f, FMath::FRandRange(0.0f, 360.0f), 0.0f);

			if (bAlignToNormal && !HitResult.ImpactNormal.IsNearlyZero())
			{
				NewInstance.PreAlignRotation = NewInstance.Rotation;
				const FRotator AlignRotation = HitResult.ImpactNormal.Rotation();
				NewInstance.Rotation = FRotator(AlignRotation.Pitch - 90.0f, AlignRotation.Yaw, 0.0f).GetNormalized();
			}

			++Placed;
		}
	}

	InstancesToAdd.Reserve(InstanceStorage.Num());
	for (const FFoliageInstance& Inst : InstanceStorage)
	{
		InstancesToAdd.Add(&Inst);
	}

	if (InstancesToAdd.Num() > 0)
	{
		FoliageInfo->AddInstances(FoliageType, InstancesToAdd);
	}

	FToolResult Result;
	Data->LastOperationStatus = FString::Printf(
		TEXT("Scattered %d instances of %s (%d skipped)"), Placed, *TypeName, Skipped);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("scatter"));
	ResultData->SetNumberField(TEXT("placed"), Placed);
	ResultData->SetNumberField(TEXT("skipped"), Skipped);
	ResultData->SetNumberField(TEXT("grid_cells"), EstimatedCount);
	Result = MakeSuccessResult(ResultData, Data->LastOperationStatus);

	if (EstimatedCount > 10000)
	{
		Result.Warnings.Add(FString::Printf(
			TEXT("Large scatter: %d grid cells. This may take a moment."), EstimatedCount));
	}

	return Result;
#else
	return MakeErrorResult(TEXT("Foliage editing requires WITH_EDITOR"));
#endif
}
