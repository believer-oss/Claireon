// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_FoliageEdit.h"
#include "Tools/ClaireonLandscapeHelpers.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"
#include "Editor.h"
#include "InstancedFoliageActor.h"
#include "InstancedFoliage.h"
#include "FoliageType.h"
#include "LandscapeProxy.h"
#include "Engine/World.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "CollisionQueryParams.h"

// Static member definitions
TMap<FString, FFoliageEditToolData> ClaireonTool_FoliageEdit::ToolData;
bool ClaireonTool_FoliageEdit::bDelegateRegistered = false;

void ClaireonTool_FoliageEdit::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("claireon.foliage_edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

FString ClaireonTool_FoliageEdit::GetName() const
{
	return TEXT("claireon.foliage_edit");
}

FString ClaireonTool_FoliageEdit::GetDescription() const
{
	return TEXT("Edit foliage instances: add types, paint/erase regions, procedural scatter.");
}

FString ClaireonTool_FoliageEdit::GetFullDescription() const
{
	return TEXT(
		"Edit foliage instances: add types, paint/erase regions, procedural scatter.\n\n"
		"Operations:\n"
		"  open - Open a session on the current level's foliage actor\n"
		"  close - Close the current session\n"
		"  status - List registered foliage types and instance counts\n"
		"  add_foliage_type - Register a foliage type asset (params: asset_path)\n"
		"  remove_foliage_type - Remove a foliage type and its instances (params: foliage_type)\n"
		"  paint - Add foliage instances in a region (params: foliage_type, center{x,y}, radius, count, min_scale, max_scale, align_to_normal, random_yaw)\n"
		"  erase - Remove foliage instances within a radius (params: center{x,y,z}, radius, foliage_type)\n"
		"  set_density - Adjust instance density in a region (params: foliage_type, center{x,y}, radius, target_density)\n"
		"  scatter - Procedural scatter with grid-jitter (params: foliage_type, bounds_min{x,y}, bounds_max{x,y}, spacing, scale_range{min,max}, align_to_normal)\n"
		"  save - Save the foliage actor package\n"
	);
}

TSharedPtr<FJsonObject> ClaireonTool_FoliageEdit::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> OpProp = MakeShared<FJsonObject>();
	OpProp->SetStringField(TEXT("type"), TEXT("string"));
	OpProp->SetStringField(TEXT("description"), TEXT("Operation to perform"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("open")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("close")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("status")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_foliage_type")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("remove_foliage_type")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("paint")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("erase")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_density")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("scatter")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("save")));
		OpProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("operation"), OpProp);

	TSharedPtr<FJsonObject> SessionProp = MakeShared<FJsonObject>();
	SessionProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionProp->SetStringField(TEXT("description"), TEXT("Session ID from open. Required for all operations except open."));
	Properties->SetObjectField(TEXT("session_id"), SessionProp);

	TSharedPtr<FJsonObject> ParamsProp = MakeShared<FJsonObject>();
	ParamsProp->SetStringField(TEXT("type"), TEXT("object"));
	ParamsProp->SetStringField(TEXT("description"), TEXT("Operation-specific parameters."));
	Properties->SetObjectField(TEXT("params"), ParamsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("operation")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_FoliageEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: operation"));
	}

	bool bSuppressOutput = false;
	Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);

	if (Operation == TEXT("open"))
	{
		TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
			? Arguments->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		return Operation_Open(Params);
	}

	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(FString::Printf(TEXT("Operation '%s' requires session_id"), *Operation));
	}

	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(SessionId);
	if (!Session)
	{
		return MakeErrorResult(FString::Printf(TEXT("Session not found or expired: %s"), *SessionId));
	}

	FFoliageEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		return MakeErrorResult(TEXT("Session tool data not found"));
	}

	if (!Data->FoliageActor.IsValid())
	{
		return MakeErrorResult(TEXT("Foliage actor no longer valid. Reopen session."));
	}

	Data->bSuppressOutput = bSuppressOutput;

	TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
		? Arguments->GetObjectField(TEXT("params"))
		: MakeShared<FJsonObject>();

	if (Operation == TEXT("close")) return Operation_Close(SessionId, Data, Params);
	if (Operation == TEXT("status")) return Operation_Status(SessionId, Data, Params);
	if (Operation == TEXT("add_foliage_type")) return Operation_AddFoliageType(SessionId, Data, Params);
	if (Operation == TEXT("remove_foliage_type")) return Operation_RemoveFoliageType(SessionId, Data, Params);
	if (Operation == TEXT("paint")) return Operation_Paint(SessionId, Data, Params);
	if (Operation == TEXT("erase")) return Operation_Erase(SessionId, Data, Params);
	if (Operation == TEXT("set_density")) return Operation_SetDensity(SessionId, Data, Params);
	if (Operation == TEXT("scatter")) return Operation_Scatter(SessionId, Data, Params);
	if (Operation == TEXT("save")) return Operation_Save(SessionId, Data, Params);

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

#if WITH_EDITOR
/** Find a UFoliageType in the foliage actor by name or asset path. */
static UFoliageType* FindFoliageTypeInActor(AInstancedFoliageActor* IFA, const FString& NameOrPath)
{
	for (auto& Pair : IFA->GetFoliageInfos())
	{
		UFoliageType* FT = Pair.Key;
		if (!FT) continue;

		if (FT->GetName().Equals(NameOrPath, ESearchCase::IgnoreCase) ||
			FT->GetPathName().Equals(NameOrPath, ESearchCase::IgnoreCase))
		{
			return FT;
		}
	}
	return nullptr;
}
#endif

// ---------------------------------------------------------------------------
// Session Management
// ---------------------------------------------------------------------------

IClaireonTool::FToolResult ClaireonTool_FoliageEdit::Operation_Open(const TSharedPtr<FJsonObject>& Params)
{
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return MakeErrorResult(TEXT("No editor world loaded"));
	}

	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&HandleSessionClosed);
		bDelegateRegistered = true;
	}

	FString Error;
	AInstancedFoliageActor* IFA = ClaireonLandscapeHelpers::GetOrCreateFoliageActor(World, Error);
	if (!IFA)
	{
		return MakeErrorResult(Error);
	}

	const FString LevelPath = World->PersistentLevel->GetPathName();
	FMCPOpenSessionResult SessionResult = FClaireonSessionManager::Get().OpenSession(LevelPath, TEXT("claireon.foliage_edit"));
	if (SessionResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		FString BlockInfo = TEXT("another tool");
		if (SessionResult.BlockingSession.IsSet())
		{
			BlockInfo = FString::Printf(TEXT("%s (session %s)"),
				*SessionResult.BlockingSession->ToolName, *SessionResult.BlockingSession->SessionId);
		}
		return MakeErrorResult(FString::Printf(TEXT("Foliage locked by %s"), *BlockInfo));
	}

	const FString SessionId = SessionResult.SessionId;
	FFoliageEditToolData& Data = ToolData.FindOrAdd(SessionId);
	Data.FoliageActor = IFA;
	Data.LastOperationStatus = TEXT("Session opened");

	return BuildStateResponse(SessionId, &Data);
}

IClaireonTool::FToolResult ClaireonTool_FoliageEdit::Operation_Close(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(ResultData, TEXT("Session closed"));
}

IClaireonTool::FToolResult ClaireonTool_FoliageEdit::Operation_Status(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	return BuildStateResponse(SessionId, Data);
}

// ---------------------------------------------------------------------------
// Type Management
// ---------------------------------------------------------------------------

IClaireonTool::FToolResult ClaireonTool_FoliageEdit::Operation_AddFoliageType(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	FString AssetPath;
	if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	UFoliageType* FoliageType = LoadObject<UFoliageType>(nullptr, *AssetPath);
	if (!FoliageType)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to load UFoliageType at '%s'. Verify the path points to a FoliageType asset."), *AssetPath));
	}

	AInstancedFoliageActor* IFA = Data->FoliageActor.Get();
	IFA->AddFoliageInfo(FoliageType);

	Data->LastOperationStatus = FString::Printf(TEXT("Added foliage type: %s"), *FoliageType->GetName());
	return BuildStateResponse(SessionId, Data);
#else
	return MakeErrorResult(TEXT("Foliage editing requires WITH_EDITOR"));
#endif
}

IClaireonTool::FToolResult ClaireonTool_FoliageEdit::Operation_RemoveFoliageType(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	FString TypeName;
	if (!Params->TryGetStringField(TEXT("foliage_type"), TypeName) || TypeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: foliage_type"));
	}

	AInstancedFoliageActor* IFA = Data->FoliageActor.Get();
	UFoliageType* FoundType = FindFoliageTypeInActor(IFA, TypeName);
	if (!FoundType)
	{
		return MakeErrorResult(FString::Printf(TEXT("Foliage type '%s' not found in this level"), *TypeName));
	}

	IFA->RemoveFoliageType(&FoundType, 1);

	Data->LastOperationStatus = FString::Printf(TEXT("Removed foliage type: %s"), *TypeName);
	return BuildStateResponse(SessionId, Data);
#else
	return MakeErrorResult(TEXT("Foliage editing requires WITH_EDITOR"));
#endif
}

// ---------------------------------------------------------------------------
// Instance Operations
// ---------------------------------------------------------------------------

IClaireonTool::FToolResult ClaireonTool_FoliageEdit::Operation_Paint(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	FString TypeName;
	if (!Params->TryGetStringField(TEXT("foliage_type"), TypeName) || TypeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: foliage_type"));
	}

	const TSharedPtr<FJsonObject>* CenterObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("center"), CenterObj) || !CenterObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: center {x, y}"));
	}
	double CenterX = 0, CenterY = 0;
	(*CenterObj)->TryGetNumberField(TEXT("x"), CenterX);
	(*CenterObj)->TryGetNumberField(TEXT("y"), CenterY);

	double Radius = 0;
	if (!Params->TryGetNumberField(TEXT("radius"), Radius) || Radius <= 0)
	{
		return MakeErrorResult(TEXT("Missing or invalid required parameter: radius"));
	}

	int32 Count = 0;
	if (!Params->TryGetNumberField(TEXT("count"), Count) || Count <= 0)
	{
		return MakeErrorResult(TEXT("Missing or invalid required parameter: count"));
	}

	double MinScale = 1.0, MaxScale = 1.0;
	Params->TryGetNumberField(TEXT("min_scale"), MinScale);
	Params->TryGetNumberField(TEXT("max_scale"), MaxScale);

	bool bAlignToNormal = true;
	Params->TryGetBoolField(TEXT("align_to_normal"), bAlignToNormal);

	bool bRandomYaw = true;
	Params->TryGetBoolField(TEXT("random_yaw"), bRandomYaw);

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

IClaireonTool::FToolResult ClaireonTool_FoliageEdit::Operation_Erase(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	const TSharedPtr<FJsonObject>* CenterObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("center"), CenterObj) || !CenterObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: center {x, y, z}"));
	}
	double CX = 0, CY = 0, CZ = 0;
	(*CenterObj)->TryGetNumberField(TEXT("x"), CX);
	(*CenterObj)->TryGetNumberField(TEXT("y"), CY);
	(*CenterObj)->TryGetNumberField(TEXT("z"), CZ);
	const FVector Center(CX, CY, CZ);

	double Radius = 0;
	if (!Params->TryGetNumberField(TEXT("radius"), Radius) || Radius <= 0)
	{
		return MakeErrorResult(TEXT("Missing or invalid required parameter: radius"));
	}

	FString TypeFilter;
	Params->TryGetStringField(TEXT("foliage_type"), TypeFilter);

	AInstancedFoliageActor* IFA = Data->FoliageActor.Get();
	const float RadiusSq = Radius * Radius;
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

IClaireonTool::FToolResult ClaireonTool_FoliageEdit::Operation_SetDensity(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	// Density adjustment is a higher-level operation that combines erase and paint
	// For now, provide a stub that returns a meaningful error directing to paint/erase
	return MakeErrorResult(TEXT("set_density is not yet implemented. Use 'paint' to add and 'erase' to remove instances manually."));
}

IClaireonTool::FToolResult ClaireonTool_FoliageEdit::Operation_Scatter(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
#if WITH_EDITOR
	FString TypeName;
	if (!Params->TryGetStringField(TEXT("foliage_type"), TypeName) || TypeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: foliage_type"));
	}

	const TSharedPtr<FJsonObject>* MinObj = nullptr;
	const TSharedPtr<FJsonObject>* MaxObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("bounds_min"), MinObj) || !MinObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: bounds_min {x, y}"));
	}
	if (!Params->TryGetObjectField(TEXT("bounds_max"), MaxObj) || !MaxObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: bounds_max {x, y}"));
	}

	double MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	(*MinObj)->TryGetNumberField(TEXT("x"), MinX);
	(*MinObj)->TryGetNumberField(TEXT("y"), MinY);
	(*MaxObj)->TryGetNumberField(TEXT("x"), MaxX);
	(*MaxObj)->TryGetNumberField(TEXT("y"), MaxY);

	double Spacing = 0;
	if (!Params->TryGetNumberField(TEXT("spacing"), Spacing) || Spacing <= 0)
	{
		return MakeErrorResult(TEXT("Missing or invalid required parameter: spacing"));
	}

	double ScaleMin = 1.0, ScaleMax = 1.0;
	const TSharedPtr<FJsonObject>* ScaleRangeObj = nullptr;
	if (Params->TryGetObjectField(TEXT("scale_range"), ScaleRangeObj) && ScaleRangeObj)
	{
		(*ScaleRangeObj)->TryGetNumberField(TEXT("min"), ScaleMin);
		(*ScaleRangeObj)->TryGetNumberField(TEXT("max"), ScaleMax);
	}

	bool bAlignToNormal = true;
	Params->TryGetBoolField(TEXT("align_to_normal"), bAlignToNormal);

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

IClaireonTool::FToolResult ClaireonTool_FoliageEdit::Operation_Save(const FString& SessionId, FFoliageEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	AInstancedFoliageActor* IFA = Data->FoliageActor.Get();
	UPackage* Package = IFA->GetOutermost();
	if (!Package)
	{
		return MakeErrorResult(TEXT("Failed to get foliage package"));
	}

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	const FSavePackageResultStruct SaveResult = UPackage::Save(Package, nullptr, *PackageFilename, SaveArgs);

	if (SaveResult.Result != ESavePackageResult::Success)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to save foliage package: %s"), *PackageFilename));
	}

	Data->LastOperationStatus = TEXT("Foliage saved");

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("save"));
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}

// ---------------------------------------------------------------------------
// BuildStateResponse
// ---------------------------------------------------------------------------

IClaireonTool::FToolResult ClaireonTool_FoliageEdit::BuildStateResponse(const FString& SessionId, FFoliageEditToolData* Data)
{
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("session_id"), SessionId);
	ResultData->SetStringField(TEXT("status"), Data->LastOperationStatus);

	if (Data->bSuppressOutput)
	{
		return MakeSuccessResult(ResultData, Data->LastOperationStatus);
	}

#if WITH_EDITOR
	AInstancedFoliageActor* IFA = Data->FoliageActor.Get();

	TArray<TSharedPtr<FJsonValue>> TypeArray;
	int32 TotalInstances = 0;

	for (auto& Pair : IFA->GetFoliageInfos())
	{
		UFoliageType* FT = Pair.Key;
		const FFoliageInfo& Info = Pair.Value.Get();

		TSharedPtr<FJsonObject> TypeJson = MakeShared<FJsonObject>();
		TypeJson->SetStringField(TEXT("name"), FT ? FT->GetName() : TEXT("Unknown"));
		TypeJson->SetStringField(TEXT("asset_path"), FT ? FT->GetPathName() : TEXT(""));
		TypeJson->SetNumberField(TEXT("instance_count"), Info.Instances.Num());
		TypeArray.Add(MakeShared<FJsonValueObject>(TypeJson));
		TotalInstances += Info.Instances.Num();
	}

	ResultData->SetArrayField(TEXT("foliage_types"), TypeArray);
	ResultData->SetNumberField(TEXT("total_instances"), TotalInstances);
#endif

	const FString Summary = FString::Printf(
		TEXT("Session %s: %s"), *SessionId, *Data->LastOperationStatus);
	return MakeSuccessResult(ResultData, Summary);
}
