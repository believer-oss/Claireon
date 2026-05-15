// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonFoliageTool_Paint.h"
#include "Tools/FToolSchemaBuilder.h"
#include "InstancedFoliageActor.h"
#include "InstancedFoliage.h"
#include "FoliageType.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonFoliageTool_Paint::GetName() const
{
	return TEXT("claireon.foliage_paint");
}

FString ClaireonFoliageTool_Paint::GetDescription() const
{
	return TEXT("Add foliage instances in a circular region. Uses line traces to place instances on ground geometry.");
}

TSharedPtr<FJsonObject> ClaireonFoliageTool_Paint::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("foliage_type"), TEXT("Name or asset path of the registered foliage type."), true);
	Builder.AddObject(TEXT("center"), TEXT("Region center {x, y} in world space."), true);
	Builder.AddNumber(TEXT("radius"), TEXT("Paint radius (uu)."), true);
	Builder.AddInteger(TEXT("count"), TEXT("Number of instances to attempt placing."), true);
	Builder.AddNumber(TEXT("min_scale"), TEXT("Minimum uniform scale (default 1.0)."));
	Builder.AddNumber(TEXT("max_scale"), TEXT("Maximum uniform scale (default 1.0)."));
	Builder.AddBoolean(TEXT("align_to_normal"), TEXT("Align instances to ground normal (default true)."));
	Builder.AddBoolean(TEXT("random_yaw"), TEXT("Randomize yaw rotation (default true)."));
	return Builder.Build();
}

FToolResult ClaireonFoliageTool_Paint::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	const TSharedPtr<FJsonObject>* CenterObj = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("center"), CenterObj) || !CenterObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: center {x, y}"));
	}
	double CenterX = 0, CenterY = 0;
	(*CenterObj)->TryGetNumberField(TEXT("x"), CenterX);
	(*CenterObj)->TryGetNumberField(TEXT("y"), CenterY);

	double Radius = 0;
	if (!Arguments->TryGetNumberField(TEXT("radius"), Radius) || Radius <= 0)
	{
		return MakeErrorResult(TEXT("Missing or invalid required parameter: radius"));
	}

	int32 Count = 0;
	if (!Arguments->TryGetNumberField(TEXT("count"), Count) || Count <= 0)
	{
		return MakeErrorResult(TEXT("Missing or invalid required parameter: count"));
	}

	double MinScale = 1.0, MaxScale = 1.0;
	Arguments->TryGetNumberField(TEXT("min_scale"), MinScale);
	Arguments->TryGetNumberField(TEXT("max_scale"), MaxScale);

	bool bAlignToNormal = true;
	Arguments->TryGetBoolField(TEXT("align_to_normal"), bAlignToNormal);

	bool bRandomYaw = true;
	Arguments->TryGetBoolField(TEXT("random_yaw"), bRandomYaw);

	AInstancedFoliageActor* IFA = Data->FoliageActor.Get();
	UFoliageType* FoliageType = FindFoliageTypeInActor(IFA, TypeName);
	if (!FoliageType)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Foliage type '%s' not found. Call add_foliage_type first."), *TypeName));
	}

	// Get the FFoliageInfo for this type
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
	InstanceStorage.Reserve(Count);

	for (int32 i = 0; i < Count; ++i)
	{
		// Uniform disk sampling
		const float Angle = FMath::FRandRange(0.0f, 2.0f * UE_PI);
		const float Dist = FMath::Sqrt(FMath::FRand()) * Radius;
		const float PosX = CenterX + Dist * FMath::Cos(Angle);
		const float PosY = CenterY + Dist * FMath::Sin(Angle);

		// Ray trace downward to find ground
		FHitResult HitResult;
		const FVector TraceStart(PosX, PosY, 100000.0);
		const FVector TraceEnd(PosX, PosY, -100000.0);
		FCollisionQueryParams QueryParams;
		QueryParams.bTraceComplex = true;

		bool bHit = World->LineTraceSingleByChannel(HitResult, TraceStart, TraceEnd, ECC_WorldStatic, QueryParams);
		if (!bHit)
		{
			++Skipped;
			continue;
		}

		FFoliageInstance& NewInstance = InstanceStorage.AddDefaulted_GetRef();
		NewInstance.Location = HitResult.ImpactPoint;

		// Random scale
		const float Scale = FMath::FRandRange(MinScale, MaxScale);
		NewInstance.DrawScale3D = FVector3f(Scale, Scale, Scale);

		// Random yaw
		if (bRandomYaw)
		{
			NewInstance.Rotation = FRotator(0.0f, FMath::FRandRange(0.0f, 360.0f), 0.0f);
		}

		// Align to normal
		if (bAlignToNormal && !HitResult.ImpactNormal.IsNearlyZero())
		{
			NewInstance.PreAlignRotation = NewInstance.Rotation;
			const FRotator AlignRotation = HitResult.ImpactNormal.Rotation();
			NewInstance.Rotation = FRotator(AlignRotation.Pitch - 90.0f, AlignRotation.Yaw, 0.0f).GetNormalized();
		}

		++Placed;
	}

	// Batch add instances
	InstancesToAdd.Reserve(InstanceStorage.Num());
	for (const FFoliageInstance& Inst : InstanceStorage)
	{
		InstancesToAdd.Add(&Inst);
	}

	if (InstancesToAdd.Num() > 0)
	{
		FoliageInfo->AddInstances(FoliageType, InstancesToAdd);
	}

	Data->LastOperationStatus = FString::Printf(
		TEXT("Painted %d instances of %s (%d skipped)"), Placed, *TypeName, Skipped);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("paint"));
	ResultData->SetNumberField(TEXT("placed"), Placed);
	ResultData->SetNumberField(TEXT("skipped"), Skipped);
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
#else
	return MakeErrorResult(TEXT("Foliage editing requires WITH_EDITOR"));
#endif
}
