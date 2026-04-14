// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LandscapeEdit.h"
#include "Tools/ClaireonLandscapeHelpers.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"
#include "Editor.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeEdit.h"
#include "LandscapeLayerInfoObject.h"
#include "LandscapeDataAccess.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "FileHelpers.h"
#include "Dom/JsonValue.h"

// Static member definitions
TMap<FString, FLandscapeEditToolData> ClaireonTool_LandscapeEdit::ToolData;
bool ClaireonTool_LandscapeEdit::bDelegateRegistered = false;

void ClaireonTool_LandscapeEdit::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("claireon.landscape_edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

FString ClaireonTool_LandscapeEdit::GetName() const
{
	return TEXT("claireon.landscape_edit");
}

FString ClaireonTool_LandscapeEdit::GetDescription() const
{
	return TEXT("Create and edit landscapes: sculpt heightmaps, paint weight layers, manage materials.");
}

FString ClaireonTool_LandscapeEdit::GetFullDescription() const
{
	return TEXT(
		"Create and edit landscapes: sculpt heightmaps, paint weight layers, manage materials.\n\n"
		"Operations:\n"
		"  open - Open a session on an existing landscape (params: landscape_name)\n"
		"  create - Create a new landscape (params: size, quads_per_section, sections_per_component, material, location, scale)\n"
		"  close - Close the current session\n"
		"  status - Get current landscape state\n"
		"  sculpt - Modify heightmap (params: center{x,y}, radius, strength, mode[raise/lower/smooth/flatten/erode], target_height)\n"
		"  paint_layer - Paint weight layer (params: layer_name, center{x,y}, radius, strength, mode[paint/erase])\n"
		"  punch_hole - Toggle landscape visibility (params: center{x,y}, radius, visible)\n"
		"  set_material - Assign landscape material (params: material_path)\n"
		"  add_layer - Add a new weight layer (params: layer_name, no_weight_blend)\n"
		"  save - Save the landscape package\n"
	);
}

TSharedPtr<FJsonObject> ClaireonTool_LandscapeEdit::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// operation - required
	TSharedPtr<FJsonObject> OpProp = MakeShared<FJsonObject>();
	OpProp->SetStringField(TEXT("type"), TEXT("string"));
	OpProp->SetStringField(TEXT("description"), TEXT("Operation to perform"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("open")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("create")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("close")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("status")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("sculpt")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("paint_layer")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("punch_hole")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("set_material")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("add_layer")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("save")));
		OpProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("operation"), OpProp);

	// session_id
	TSharedPtr<FJsonObject> SessionProp = MakeShared<FJsonObject>();
	SessionProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionProp->SetStringField(TEXT("description"), TEXT("Session ID from open/create. Required for all operations except open/create."));
	Properties->SetObjectField(TEXT("session_id"), SessionProp);

	// params
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

IClaireonTool::FToolResult ClaireonTool_LandscapeEdit::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Operation;
	if (!Arguments->TryGetStringField(TEXT("operation"), Operation) || Operation.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: operation"));
	}

	bool bSuppressOutput = false;
	Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);

	// Session-less operations
	if (Operation == TEXT("open"))
	{
		TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
			? Arguments->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		return Operation_Open(Params);
	}
	if (Operation == TEXT("create"))
	{
		TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
			? Arguments->GetObjectField(TEXT("params"))
			: MakeShared<FJsonObject>();
		return Operation_Create(Params);
	}

	// Session-required operations
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

	FLandscapeEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		return MakeErrorResult(TEXT("Session tool data not found"));
	}

	if (!Data->IsValid())
	{
		return MakeErrorResult(TEXT("Landscape no longer valid. Close and reopen session."));
	}

	Data->bSuppressOutput = bSuppressOutput;

	TSharedPtr<FJsonObject> Params = Arguments->HasField(TEXT("params"))
		? Arguments->GetObjectField(TEXT("params"))
		: MakeShared<FJsonObject>();

	if (Operation == TEXT("close")) return Operation_Close(SessionId, Data, Params);
	if (Operation == TEXT("status")) return Operation_Status(SessionId, Data, Params);
	if (Operation == TEXT("sculpt")) return Operation_Sculpt(SessionId, Data, Params);
	if (Operation == TEXT("paint_layer")) return Operation_PaintLayer(SessionId, Data, Params);
	if (Operation == TEXT("punch_hole")) return Operation_PunchHole(SessionId, Data, Params);
	if (Operation == TEXT("set_material")) return Operation_SetMaterial(SessionId, Data, Params);
	if (Operation == TEXT("add_layer")) return Operation_AddLayer(SessionId, Data, Params);
	if (Operation == TEXT("save")) return Operation_Save(SessionId, Data, Params);

	return MakeErrorResult(FString::Printf(TEXT("Unknown operation: %s"), *Operation));
}

// ---------------------------------------------------------------------------
// Helper to extract FVector from JSON
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Session Management
// ---------------------------------------------------------------------------

IClaireonTool::FToolResult ClaireonTool_LandscapeEdit::Operation_Open(const TSharedPtr<FJsonObject>& Params)
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

	// Register delegate on first use
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&HandleSessionClosed);
		bDelegateRegistered = true;
	}

	FString LandscapeName;
	if (!Params->TryGetStringField(TEXT("landscape_name"), LandscapeName) || LandscapeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: landscape_name"));
	}

	TArray<TPair<ULandscapeInfo*, ALandscapeProxy*>> Landscapes =
		ClaireonLandscapeHelpers::FindLandscapeInWorld(World, LandscapeName);

	if (Landscapes.Num() == 0)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("No landscape matching '%s' found in the current world"), *LandscapeName));
	}
	if (Landscapes.Num() > 1)
	{
		TArray<FString> Names;
		for (const auto& L : Landscapes)
		{
			Names.Add(L.Value->GetActorLabel());
		}
		return MakeErrorResult(FString::Printf(
			TEXT("Multiple landscapes match '%s': %s. Provide a more specific name."),
			*LandscapeName, *FString::Join(Names, TEXT(", "))));
	}

	ULandscapeInfo* LandscapeInfo = Landscapes[0].Key;
	ALandscapeProxy* Proxy = Landscapes[0].Value;
	const FString ActorPath = Proxy->GetPathName();

	// Acquire session lock
	FMCPOpenSessionResult SessionResult = FClaireonSessionManager::Get().OpenSession(ActorPath, TEXT("claireon.landscape_edit"));
	if (SessionResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		FString BlockInfo = TEXT("another tool");
		if (SessionResult.BlockingSession.IsSet())
		{
			BlockInfo = FString::Printf(TEXT("%s (session %s)"),
				*SessionResult.BlockingSession->ToolName, *SessionResult.BlockingSession->SessionId);
		}
		return MakeErrorResult(FString::Printf(TEXT("Landscape is locked by %s"), *BlockInfo));
	}

	const FString SessionId = SessionResult.SessionId;

	FLandscapeEditToolData& Data = ToolData.FindOrAdd(SessionId);
	Data.LandscapeProxy = Proxy;
	Data.LandscapeInfo = LandscapeInfo;
	Data.LastOperationStatus = TEXT("Session opened");

	return BuildStateResponse(SessionId, &Data);
}

IClaireonTool::FToolResult ClaireonTool_LandscapeEdit::Operation_Create(const TSharedPtr<FJsonObject>& Params)
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

	// Extract parameters with defaults
	int32 Size = 505;
	Params->TryGetNumberField(TEXT("size"), Size);

	int32 QuadsPerSection = 63;
	Params->TryGetNumberField(TEXT("quads_per_section"), QuadsPerSection);

	int32 SectionsPerComponent = 1;
	Params->TryGetNumberField(TEXT("sections_per_component"), SectionsPerComponent);

	const TSharedPtr<FJsonObject>* LocationObj = nullptr;
	Params->TryGetObjectField(TEXT("location"), LocationObj);
	const FVector Location = ExtractVector(LocationObj ? *LocationObj : nullptr, FVector::ZeroVector);

	const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
	Params->TryGetObjectField(TEXT("scale"), ScaleObj);
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
	if (Params->TryGetStringField(TEXT("material"), MaterialPath) && !MaterialPath.IsEmpty())
	{
		UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
		if (Material)
		{
			NewLandscape->LandscapeMaterial = Material;
			NewLandscape->UpdateAllComponentMaterialInstances();
		}
		else
		{
			UE_LOG(LogClaireon, Warning, TEXT("LandscapeEdit::Create: Material '%s' not found"), *MaterialPath);
		}
	}

	// Get the landscape info for the new landscape
	ULandscapeInfo* LandscapeInfo = NewLandscape->GetLandscapeInfo();

	const FString ActorPath = NewLandscape->GetPathName();
	FMCPOpenSessionResult SessionResult = FClaireonSessionManager::Get().OpenSession(ActorPath, TEXT("claireon.landscape_edit"));
	const FString SessionId = SessionResult.SessionId;

	FLandscapeEditToolData& Data = ToolData.FindOrAdd(SessionId);
	Data.LandscapeProxy = NewLandscape;
	Data.LandscapeInfo = LandscapeInfo;
	Data.LastOperationStatus = FString::Printf(TEXT("Created %dx%d landscape"), Size, Size);

	return BuildStateResponse(SessionId, &Data);
}

IClaireonTool::FToolResult ClaireonTool_LandscapeEdit::Operation_Close(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FClaireonSessionManager::Get().CloseSession(SessionId);
	ToolData.Remove(SessionId);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("status"), TEXT("closed"));
	return MakeSuccessResult(ResultData, TEXT("Session closed"));
}

IClaireonTool::FToolResult ClaireonTool_LandscapeEdit::Operation_Status(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	return BuildStateResponse(SessionId, Data);
}

// ---------------------------------------------------------------------------
// Editing Operations
// ---------------------------------------------------------------------------

IClaireonTool::FToolResult ClaireonTool_LandscapeEdit::Operation_Sculpt(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	// Extract parameters
	const TSharedPtr<FJsonObject>* CenterObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("center"), CenterObj) || !CenterObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: center {x, y}"));
	}
	double CenterWorldX = 0, CenterWorldY = 0;
	(*CenterObj)->TryGetNumberField(TEXT("x"), CenterWorldX);
	(*CenterObj)->TryGetNumberField(TEXT("y"), CenterWorldY);

	double Radius = 0;
	if (!Params->TryGetNumberField(TEXT("radius"), Radius) || Radius <= 0)
	{
		return MakeErrorResult(TEXT("Missing or invalid required parameter: radius (must be > 0)"));
	}

	double Strength = 1.0;
	Params->TryGetNumberField(TEXT("strength"), Strength);

	FString ModeStr = TEXT("raise");
	Params->TryGetStringField(TEXT("mode"), ModeStr);

	double TargetHeight = 0.0;
	Params->TryGetNumberField(TEXT("target_height"), TargetHeight);

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

IClaireonTool::FToolResult ClaireonTool_LandscapeEdit::Operation_PaintLayer(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString LayerName;
	if (!Params->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: layer_name"));
	}

	const TSharedPtr<FJsonObject>* CenterObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("center"), CenterObj) || !CenterObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: center {x, y}"));
	}
	double CenterWorldX = 0, CenterWorldY = 0;
	(*CenterObj)->TryGetNumberField(TEXT("x"), CenterWorldX);
	(*CenterObj)->TryGetNumberField(TEXT("y"), CenterWorldY);

	double Radius = 0;
	if (!Params->TryGetNumberField(TEXT("radius"), Radius) || Radius <= 0)
	{
		return MakeErrorResult(TEXT("Missing or invalid required parameter: radius (must be > 0)"));
	}

	double Strength = 1.0;
	Params->TryGetNumberField(TEXT("strength"), Strength);

	FString ModeStr = TEXT("paint");
	Params->TryGetStringField(TEXT("mode"), ModeStr);
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

IClaireonTool::FToolResult ClaireonTool_LandscapeEdit::Operation_PunchHole(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	const TSharedPtr<FJsonObject>* CenterObj = nullptr;
	if (!Params->TryGetObjectField(TEXT("center"), CenterObj) || !CenterObj)
	{
		return MakeErrorResult(TEXT("Missing required parameter: center {x, y}"));
	}
	double CenterWorldX = 0, CenterWorldY = 0;
	(*CenterObj)->TryGetNumberField(TEXT("x"), CenterWorldX);
	(*CenterObj)->TryGetNumberField(TEXT("y"), CenterWorldY);

	double Radius = 0;
	if (!Params->TryGetNumberField(TEXT("radius"), Radius) || Radius <= 0)
	{
		return MakeErrorResult(TEXT("Missing or invalid required parameter: radius (must be > 0)"));
	}

	bool bVisible = false;
	Params->TryGetBoolField(TEXT("visible"), bVisible);

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

IClaireonTool::FToolResult ClaireonTool_LandscapeEdit::Operation_SetMaterial(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath;
	if (!Params->TryGetStringField(TEXT("material_path"), MaterialPath) || MaterialPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: material_path"));
	}

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, *MaterialPath);
	if (!Material)
	{
		return MakeErrorResult(FString::Printf(TEXT("Material '%s' not found"), *MaterialPath));
	}

	ALandscapeProxy* Proxy = Data->LandscapeProxy.Get();
	Proxy->LandscapeMaterial = Material;
	Proxy->UpdateAllComponentMaterialInstances();

	Data->LastOperationStatus = FString::Printf(TEXT("Set material to %s"), *MaterialPath);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("set_material"));
	ResultData->SetStringField(TEXT("material_path"), MaterialPath);
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}

IClaireonTool::FToolResult ClaireonTool_LandscapeEdit::Operation_AddLayer(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString LayerName;
	if (!Params->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: layer_name"));
	}

	bool bNoWeightBlend = false;
	Params->TryGetBoolField(TEXT("no_weight_blend"), bNoWeightBlend);

	ULandscapeLayerInfoObject* LayerInfo = NewObject<ULandscapeLayerInfoObject>();
	LayerInfo->LayerName = FName(*LayerName);
	LayerInfo->bNoWeightBlend = bNoWeightBlend;

	ULandscapeInfo* LandscapeInfo = Data->LandscapeInfo.Get();
	ALandscapeProxy* Proxy = Data->LandscapeProxy.Get();

	LandscapeInfo->Layers.Add(FLandscapeInfoLayerSettings(LayerInfo, Proxy));

	// Build updated layer list
	TArray<TSharedPtr<FJsonValue>> LayerArray;
	for (const FLandscapeInfoLayerSettings& LayerSettings : LandscapeInfo->Layers)
	{
		LayerArray.Add(MakeShared<FJsonValueString>(LayerSettings.GetLayerName().ToString()));
	}

	Data->LastOperationStatus = FString::Printf(TEXT("Added layer '%s'"), *LayerName);

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("add_layer"));
	ResultData->SetStringField(TEXT("layer_name"), LayerName);
	ResultData->SetArrayField(TEXT("layers"), LayerArray);
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}

IClaireonTool::FToolResult ClaireonTool_LandscapeEdit::Operation_Save(const FString& SessionId, FLandscapeEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	ALandscapeProxy* Proxy = Data->LandscapeProxy.Get();
	UPackage* Package = Proxy->GetOutermost();
	if (!Package)
	{
		return MakeErrorResult(TEXT("Failed to get landscape package"));
	}

	const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Standalone;
	const FSavePackageResultStruct SaveResult = UPackage::Save(Package, nullptr, *PackageFilename, SaveArgs);

	if (SaveResult.Result != ESavePackageResult::Success)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to save landscape package: %s"), *PackageFilename));
	}

	Data->LastOperationStatus = TEXT("Landscape saved");

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("operation"), TEXT("save"));
	ResultData->SetStringField(TEXT("package"), Package->GetName());
	return MakeSuccessResult(ResultData, Data->LastOperationStatus);
}

IClaireonTool::FToolResult ClaireonTool_LandscapeEdit::BuildStateResponse(const FString& SessionId, FLandscapeEditToolData* Data)
{
	if (Data->bSuppressOutput)
	{
		TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
		ResultData->SetStringField(TEXT("session_id"), SessionId);
		ResultData->SetStringField(TEXT("status"), Data->LastOperationStatus);
		return MakeSuccessResult(ResultData, Data->LastOperationStatus);
	}

	TSharedPtr<FJsonObject> LandscapeJson = ClaireonLandscapeHelpers::BuildLandscapeInfoJson(
		Data->LandscapeInfo.Get(), TEXT("full"));

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("session_id"), SessionId);
	ResultData->SetStringField(TEXT("status"), Data->LastOperationStatus);
	if (LandscapeJson)
	{
		ResultData->SetObjectField(TEXT("landscape"), LandscapeJson);
	}

	const FString Summary = FString::Printf(
		TEXT("Session %s: %s"), *SessionId, *Data->LastOperationStatus);
	return MakeSuccessResult(ResultData, Summary);
}
