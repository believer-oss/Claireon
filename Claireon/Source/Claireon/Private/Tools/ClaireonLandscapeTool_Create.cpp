// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeTool_Create.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSessionManager.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "Editor.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeDataAccess.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"

using FToolResult = IClaireonTool::FToolResult;

// Helper to extract FVector from JSON
static FVector ExtractVector(const TSharedPtr<FJsonObject>& Obj, const FVector& Default)
{
	if (!Obj) return Default;
	FVector Result = Default;
	double Val;
	if (Obj->TryGetNumberField(TEXT("x"), Val)) Result.X = Val;
	if (Obj->TryGetNumberField(TEXT("y"), Val)) Result.Y = Val;
	if (Obj->TryGetNumberField(TEXT("z"), Val)) Result.Z = Val;
	return Result;
}

FString ClaireonLandscapeTool_Create::GetName() const
{
	return TEXT("claireon.landscape_create");
}

FString ClaireonLandscapeTool_Create::GetDescription() const
{
	return TEXT("Create a new landscape actor in the current world and open a session on it.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeTool_Create::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddInteger(TEXT("size"), TEXT("Landscape resolution in quads (default 505)."));
	Builder.AddInteger(TEXT("quads_per_section"), TEXT("Quads per section (default 63)."));
	Builder.AddInteger(TEXT("sections_per_component"), TEXT("Sections per component (default 1)."));
	Builder.AddString(TEXT("material"), TEXT("Optional path to a UMaterialInterface to apply to the landscape."));
	Builder.AddObject(TEXT("location"), TEXT("World location {x, y, z} (default 0,0,0)."));
	Builder.AddObject(TEXT("scale"), TEXT("World scale {x, y, z} (default 100,100,100)."));
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("When true, response omits the full landscape info block."));
	return Builder.Build();
}

FToolResult ClaireonLandscapeTool_Create::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	EnsureDelegateRegistered();

	// Extract parameters with defaults
	int32 Size = 505;
	Arguments->TryGetNumberField(TEXT("size"), Size);

	int32 QuadsPerSection = 63;
	Arguments->TryGetNumberField(TEXT("quads_per_section"), QuadsPerSection);

	int32 SectionsPerComponent = 1;
	Arguments->TryGetNumberField(TEXT("sections_per_component"), SectionsPerComponent);

	const TSharedPtr<FJsonObject>* LocationObj = nullptr;
	Arguments->TryGetObjectField(TEXT("location"), LocationObj);
	const FVector Location = ExtractVector(LocationObj ? *LocationObj : nullptr, FVector::ZeroVector);

	const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
	Arguments->TryGetObjectField(TEXT("scale"), ScaleObj);
	const FVector Scale = ExtractVector(ScaleObj ? *ScaleObj : nullptr, FVector(100.0, 100.0, 100.0));

	// Spawn landscape actor
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ALandscape* NewLandscape = World->SpawnActor<ALandscape>(SpawnParams);

	if (!NewLandscape)
	{
		return MakeErrorResult(TEXT("Failed to spawn ALandscape actor"));
	}

	NewLandscape->SetActorLocation(Location);
	NewLandscape->SetActorScale3D(Scale);

	// Initialize flat heightmap
	TArray<uint16> HeightData;
	HeightData.SetNumUninitialized(Size * Size);
	FMemory::Memset(HeightData.GetData(), 0, Size * Size * sizeof(uint16));
	for (int32 i = 0; i < HeightData.Num(); ++i)
	{
		HeightData[i] = 32768; // Zero height
	}

	const int32 MinX = 0;
	const int32 MinY = 0;
	const int32 MaxX = Size - 1;
	const int32 MaxY = Size - 1;

	FGuid ImportGuid = FGuid::NewGuid();
	TMap<FGuid, TArray<uint16>> HeightDataMap;
	HeightDataMap.Add(ImportGuid, MoveTemp(HeightData));

	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerInfos;
	MaterialLayerInfos.Add(ImportGuid, TArray<FLandscapeImportLayerInfo>());

	TArray<FLandscapeLayer> EmptyLayers;
	NewLandscape->Import(
		ImportGuid,
		MinX, MinY, MaxX, MaxY,
		SectionsPerComponent, QuadsPerSection,
		HeightDataMap, nullptr,
		MaterialLayerInfos,
		ELandscapeImportAlphamapType::Additive,
		EmptyLayers);

	// Apply material if specified
	FString MaterialPath;
	if (Arguments->TryGetStringField(TEXT("material"), MaterialPath) && !MaterialPath.IsEmpty())
	{
		// Normalize caller-provided path through the central resolver before LoadObject.
		const auto MaterialPathResolve = ClaireonPathResolver::Resolve(MaterialPath);
		if (!MaterialPathResolve.bSuccess)
		{
			UE_LOG(LogClaireon, Warning, TEXT("LandscapeCreate: Material path resolve failed: %s"), *MaterialPathResolve.Error);
		}
		else
		{
			MaterialPath = MaterialPathResolve.ResolvedPath.Path;
			UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
			if (Material)
			{
				NewLandscape->LandscapeMaterial = Material;
				NewLandscape->UpdateAllComponentMaterialInstances();
			}
			else
			{
				UE_LOG(LogClaireon, Warning, TEXT("LandscapeCreate: Material '%s' not found"), *MaterialPath);
			}
		}
	}

	// Get the landscape info for the new landscape
	ULandscapeInfo* LandscapeInfo = NewLandscape->GetLandscapeInfo();

	const FString ActorPath = NewLandscape->GetPathName();
	FMCPOpenSessionResult SessionResult = FClaireonSessionManager::Get().OpenSession(ActorPath, LandscapeSessionToolName);
	const FString SessionId = SessionResult.SessionId;

	FLandscapeEditToolData& Data = ToolData.FindOrAdd(SessionId);
	Data.LandscapeProxy = NewLandscape;
	Data.LandscapeInfo = LandscapeInfo;
	Data.LastOperationStatus = FString::Printf(TEXT("Created %dx%d landscape"), Size, Size);

	bool bSuppressOutput = false;
	Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);
	Data.bSuppressOutput = bSuppressOutput;

	return BuildStateResponse(SessionId, &Data);
}
