// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonFoliageTool_Erase.h"
#include "Tools/FToolSchemaBuilder.h"
#include "InstancedFoliageActor.h"
#include "InstancedFoliage.h"
#include "FoliageType.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonFoliageTool_Erase::GetOperation() const { return TEXT("erase"); }

FString ClaireonFoliageTool_Erase::GetDescription() const
{
    return TEXT("Remove foliage instances within a sphere. Optionally filter by foliage type. Session-mode tool: open via foliage_open first.");
}

TSharedPtr<FJsonObject> ClaireonFoliageTool_Erase::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddObject(TEXT("center"), TEXT("Sphere center {x, y, z} in world space."), true);
	Builder.AddNumber(TEXT("radius"), TEXT("Erase radius (uu)."), true);
	Builder.AddString(TEXT("foliage_type"), TEXT("Optional name or asset path filter; when empty, erases all types."));
	return Builder.Build();
}

FToolResult ClaireonFoliageTool_Erase::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if WITH_EDITOR
	FString SessionId;
	FFoliageEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	const TSharedPtr<FJsonObject>* CenterObj = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("center"), CenterObj) || !CenterObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: center {x, y, z}"));
	}
	double CX = 0, CY = 0, CZ = 0;
	(*CenterObj)->TryGetNumberField(TEXT("x"), CX);
	(*CenterObj)->TryGetNumberField(TEXT("y"), CY);
	(*CenterObj)->TryGetNumberField(TEXT("z"), CZ);
	const FVector Center(CX, CY, CZ);

	double Radius = 0;
	if (!Arguments->TryGetNumberField(TEXT("radius"), Radius) || Radius <= 0)
	{
		return MakeErrorResult(TEXT("Missing or invalid required parameter: radius"));
	}

	FString TypeFilter;
	Arguments->TryGetStringField(TEXT("foliage_type"), TypeFilter);

	AInstancedFoliageActor* IFA = Data->FoliageActor.Get();
	int32 TotalRemoved = 0;

	for (const auto& Pair : IFA->GetFoliageInfos())
	{
		UFoliageType* FT = Pair.Key;

		// If a type filter is set, skip non-matching types
		if (!TypeFilter.IsEmpty())
		{
			if (!FT->GetName().Equals(TypeFilter, ESearchCase::IgnoreCase) &&
				!FT->GetPathName().Equals(TypeFilter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		// Get mutable info for removal
		FFoliageInfo* Info = IFA->FindInfo(FT);
		if (!Info) continue;

		// Use GetInstancesInsideSphere for efficient spatial query
		const FSphere EraseSphere(Center, Radius);
		TArray<int32> IndicesToRemove;
		Info->GetInstancesInsideSphere(EraseSphere, IndicesToRemove);

		if (IndicesToRemove.Num() > 0)
		{
			Info->RemoveInstances(IndicesToRemove, true /*RebuildFoliageTree*/);
			TotalRemoved += IndicesToRemove.Num();
		}
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Erased %d foliage instances"), TotalRemoved);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("erase"));
	ResultData->SetNumberField(TEXT("removed"), TotalRemoved);
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
#else
	return MakeErrorResult(TEXT("Foliage editing requires WITH_EDITOR"));
#endif
}
