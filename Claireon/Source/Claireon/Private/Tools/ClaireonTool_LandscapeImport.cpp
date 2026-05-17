// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LandscapeImport.h"
#include "Tools/ClaireonLandscapeHelpers.h"
#include "ClaireonLog.h"
#include "Editor.h"
#include "LandscapeImportHelper.h"
#include "LandscapeFileFormatInterface.h"
#include "Landscape.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"
#include "LandscapeEdit.h"
#include "LandscapeLayerInfoObject.h"
#include "Engine/World.h"
#include "Misc/Paths.h"

FString ClaireonTool_LandscapeImport::GetCategory() const { return TEXT("landscape"); }
FString ClaireonTool_LandscapeImport::GetOperation() const { return TEXT("import"); }

FString ClaireonTool_LandscapeImport::GetDescription() const
{
	return TEXT("Import heightmaps and weightmaps from external files (R16, PNG, RAW).");
}

TSharedPtr<FJsonObject> ClaireonTool_LandscapeImport::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// type - required
	TSharedPtr<FJsonObject> TypeProp = MakeShared<FJsonObject>();
	TypeProp->SetStringField(TEXT("type"), TEXT("string"));
	TypeProp->SetStringField(TEXT("description"), TEXT("Import type: 'heightmap' or 'weightmap'"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("heightmap")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("weightmap")));
		TypeProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("type"), TypeProp);

	// file_path - required
	TSharedPtr<FJsonObject> FileProp = MakeShared<FJsonObject>();
	FileProp->SetStringField(TEXT("type"), TEXT("string"));
	FileProp->SetStringField(TEXT("description"), TEXT("Absolute path to the file on disk"));
	Properties->SetObjectField(TEXT("file_path"), FileProp);

	// landscape_name - conditional
	TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"), TEXT("Name or partial match of target landscape. Required for weightmap or importing into existing landscape."));
	Properties->SetObjectField(TEXT("landscape_name"), NameProp);

	// layer_name - conditional
	TSharedPtr<FJsonObject> LayerProp = MakeShared<FJsonObject>();
	LayerProp->SetStringField(TEXT("type"), TEXT("string"));
	LayerProp->SetStringField(TEXT("description"), TEXT("Target weight layer name. Required when type is 'weightmap'."));
	Properties->SetObjectField(TEXT("layer_name"), LayerProp);

	// create_landscape - optional
	TSharedPtr<FJsonObject> CreateProp = MakeShared<FJsonObject>();
	CreateProp->SetStringField(TEXT("type"), TEXT("boolean"));
	CreateProp->SetStringField(TEXT("description"), TEXT("If true and type is 'heightmap', create a new landscape from the imported data. Default: false."));
	Properties->SetObjectField(TEXT("create_landscape"), CreateProp);

	// location - optional
	TSharedPtr<FJsonObject> LocationProp = MakeShared<FJsonObject>();
	LocationProp->SetStringField(TEXT("type"), TEXT("object"));
	LocationProp->SetStringField(TEXT("description"), TEXT("World location {x, y, z} for new landscape. Only used with create_landscape."));
	Properties->SetObjectField(TEXT("location"), LocationProp);

	// scale - optional
	TSharedPtr<FJsonObject> ScaleProp = MakeShared<FJsonObject>();
	ScaleProp->SetStringField(TEXT("type"), TEXT("object"));
	ScaleProp->SetStringField(TEXT("description"), TEXT("Scale {x, y, z} for new landscape. Only used with create_landscape."));
	Properties->SetObjectField(TEXT("scale"), ScaleProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("type")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("file_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

namespace ClaireonTool_LandscapeImportInternal
{

// Helper to extract FVector from JSON object with {x, y, z} fields, falling back to defaults
FVector ExtractVectorFromJson(const TSharedPtr<FJsonObject>& Obj, const FVector& Default)
{
	if (!Obj) return Default;
	FVector Result = Default;
	double Val;
	if (Obj->TryGetNumberField(TEXT("x"), Val)) Result.X = Val;
	if (Obj->TryGetNumberField(TEXT("y"), Val)) Result.Y = Val;
	if (Obj->TryGetNumberField(TEXT("z"), Val)) Result.Z = Val;
	return Result;
}

}  // namespace ClaireonTool_LandscapeImportInternal

IClaireonTool::FToolResult ClaireonTool_LandscapeImport::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	// --- Parameter extraction ---
	FString ImportType;
	if (!Arguments->TryGetStringField(TEXT("type"), ImportType) || ImportType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: type"));
	}

	FString FilePath;
	if (!Arguments->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: file_path"));
	}

	FString LandscapeName;
	Arguments->TryGetStringField(TEXT("landscape_name"), LandscapeName);

	FString LayerName;
	Arguments->TryGetStringField(TEXT("layer_name"), LayerName);

	bool bCreateLandscape = false;
	Arguments->TryGetBoolField(TEXT("create_landscape"), bCreateLandscape);

	// --- Validation ---
	if (!FPaths::FileExists(FilePath))
	{
		return MakeErrorResult(FString::Printf(TEXT("File not found: '%s'"), *FilePath));
	}

	const FString Extension = FPaths::GetExtension(FilePath).ToLower();
	if (Extension != TEXT("r16") && Extension != TEXT("raw") && Extension != TEXT("png"))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Unsupported file format '.%s'. Supported: .r16, .raw, .png"), *Extension));
	}

	if (ImportType == TEXT("weightmap") && LayerName.IsEmpty())
	{
		return MakeErrorResult(TEXT("layer_name is required for weightmap import"));
	}

	if ((ImportType == TEXT("weightmap") || !bCreateLandscape) && LandscapeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("landscape_name is required when importing into an existing landscape"));
	}

	// --- Heightmap Import ---
	if (ImportType == TEXT("heightmap"))
	{
		// Get import descriptor to validate file and determine resolution
		FLandscapeImportDescriptor ImportDescriptor;
		FText Message;
		ELandscapeImportResult DescResult = FLandscapeImportHelper::GetHeightmapImportDescriptor(
			FilePath, true /*bSingleFile*/, false /*bFlipYAxis*/, ImportDescriptor, Message);

		if (DescResult == ELandscapeImportResult::Error)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to read heightmap: %s"), *Message.ToString()));
		}

		if (ImportDescriptor.ImportResolutions.Num() == 0)
		{
			return MakeErrorResult(TEXT("Heightmap import descriptor has no valid resolutions"));
		}

		const int32 ImportWidth = ImportDescriptor.ImportResolutions[0].Width;
		const int32 ImportHeight = ImportDescriptor.ImportResolutions[0].Height;

		// Load actual heightmap data
		TArray<uint16> HeightData;
		ELandscapeImportResult DataResult = FLandscapeImportHelper::GetHeightmapImportData(
			ImportDescriptor, 0 /*DescriptorIndex*/, HeightData, Message);

		if (DataResult == ELandscapeImportResult::Error)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to load heightmap data: %s"), *Message.ToString()));
		}

		if (bCreateLandscape)
		{
			// --- Create new landscape from heightmap ---
			int32 QuadsPerSection = 63;
			int32 SectionsPerComponent = 1;
			FIntPoint ComponentCount;
			FLandscapeImportHelper::ChooseBestComponentSizeForImport(
				ImportWidth, ImportHeight, QuadsPerSection, SectionsPerComponent, ComponentCount);

			// Extract location and scale
			const TSharedPtr<FJsonObject>* LocationObj = nullptr;
			Arguments->TryGetObjectField(TEXT("location"), LocationObj);
			const FVector Location = ClaireonTool_LandscapeImportInternal::ExtractVectorFromJson(LocationObj ? *LocationObj : nullptr, FVector::ZeroVector);

			const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
			Arguments->TryGetObjectField(TEXT("scale"), ScaleObj);
			const FVector Scale = ClaireonTool_LandscapeImportInternal::ExtractVectorFromJson(ScaleObj ? *ScaleObj : nullptr, FVector(100.0, 100.0, 100.0));

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

			// Import heightmap data via the landscape's Import method
			const int32 MinX = 0;
			const int32 MinY = 0;
			const int32 MaxX = ImportWidth - 1;
			const int32 MaxY = ImportHeight - 1;
			const int32 SubsectionSizeQuads = QuadsPerSection;
			const int32 NumSubsections = SectionsPerComponent;

			TMap<FGuid, TArray<uint16>> HeightDataMap;
			FGuid ImportGuid = FGuid::NewGuid();
			HeightDataMap.Add(ImportGuid, MoveTemp(HeightData));

			TMap<FGuid, TArray<FLandscapeImportLayerInfo>> MaterialLayerInfos;
			MaterialLayerInfos.Add(ImportGuid, TArray<FLandscapeImportLayerInfo>());

			TArray<FLandscapeLayer> EmptyLayers;
			NewLandscape->Import(
				ImportGuid,
				MinX, MinY, MaxX, MaxY,
				NumSubsections, SubsectionSizeQuads,
				HeightDataMap, nullptr /*HeightmapFileName*/,
				MaterialLayerInfos,
				ELandscapeImportAlphamapType::Additive,
				EmptyLayers);

			// Build result
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("type"), TEXT("heightmap"));
			Data->SetStringField(TEXT("file_path"), FilePath);

			TSharedPtr<FJsonObject> Resolution = MakeShared<FJsonObject>();
			Resolution->SetNumberField(TEXT("width"), ImportWidth);
			Resolution->SetNumberField(TEXT("height"), ImportHeight);
			Data->SetObjectField(TEXT("resolution"), Resolution);

			Data->SetStringField(TEXT("landscape_name"), NewLandscape->GetActorLabel());
			Data->SetStringField(TEXT("landscape_path"), NewLandscape->GetPathName());
			Data->SetBoolField(TEXT("created_new"), true);

			FString Summary = FString::Printf(
				TEXT("Created new landscape from heightmap (%dx%d) at %s"),
				ImportWidth, ImportHeight, *FilePath);

			FToolResult Result = MakeSuccessResult(Data, Summary);
			if (DescResult == ELandscapeImportResult::Warning)
			{
				Result.Warnings.Add(Message.ToString());
			}
			return Result;
		}
		else
		{
			// --- Import into existing landscape ---
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

			// Check resolution matches
			int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
			if (LandscapeInfo->GetLandscapeExtent(MinX, MinY, MaxX, MaxY))
			{
				const int32 LandscapeWidth = MaxX - MinX + 1;
				const int32 LandscapeHeight = MaxY - MinY + 1;
				if (ImportWidth != LandscapeWidth || ImportHeight != LandscapeHeight)
				{
					return MakeErrorResult(FString::Printf(
						TEXT("Import resolution %dx%d does not match landscape %dx%d"),
						ImportWidth, ImportHeight, LandscapeWidth, LandscapeHeight));
				}
			}

			// Write heightmap data
			FLandscapeEditDataInterface EditInterface(LandscapeInfo);
			EditInterface.SetHeightData(
				MinX, MinY, MaxX, MaxY,
				HeightData.GetData(), 0 /*Stride*/,
				true /*CalcNormals*/);

			// Build result
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("type"), TEXT("heightmap"));
			Data->SetStringField(TEXT("file_path"), FilePath);

			TSharedPtr<FJsonObject> Resolution = MakeShared<FJsonObject>();
			Resolution->SetNumberField(TEXT("width"), ImportWidth);
			Resolution->SetNumberField(TEXT("height"), ImportHeight);
			Data->SetObjectField(TEXT("resolution"), Resolution);

			Data->SetStringField(TEXT("landscape_name"), Proxy->GetActorLabel());
			Data->SetStringField(TEXT("landscape_path"), Proxy->GetPathName());
			Data->SetBoolField(TEXT("created_new"), false);

			FString Summary = FString::Printf(
				TEXT("Imported heightmap (%dx%d) from %s into %s"),
				ImportWidth, ImportHeight, *FilePath, *Proxy->GetActorLabel());

			FToolResult Result = MakeSuccessResult(Data, Summary);
			if (DescResult == ELandscapeImportResult::Warning)
			{
				Result.Warnings.Add(Message.ToString());
			}
			return Result;
		}
	}

	// --- Weightmap Import ---
	if (ImportType == TEXT("weightmap"))
	{
		// Find target landscape
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

		// Find the target layer
		ULandscapeLayerInfoObject* TargetLayerInfo = nullptr;
		TArray<FString> AvailableLayerNames;
		for (const FLandscapeInfoLayerSettings& LayerSettings : LandscapeInfo->Layers)
		{
			const FString CurrentLayerName = LayerSettings.GetLayerName().ToString();
			AvailableLayerNames.Add(CurrentLayerName);
			if (CurrentLayerName.Equals(LayerName, ESearchCase::IgnoreCase) && LayerSettings.LayerInfoObj)
			{
				TargetLayerInfo = LayerSettings.LayerInfoObj;
			}
		}

		if (!TargetLayerInfo)
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Layer '%s' not found. Available: %s"),
				*LayerName, *FString::Join(AvailableLayerNames, TEXT(", "))));
		}

		// Get weightmap import descriptor
		FLandscapeImportDescriptor ImportDescriptor;
		FText Message;
		ELandscapeImportResult DescResult = FLandscapeImportHelper::GetWeightmapImportDescriptor(
			FilePath, true /*bSingleFile*/, false /*bFlipYAxis*/, FName(*LayerName),
			ImportDescriptor, Message);

		if (DescResult == ELandscapeImportResult::Error)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to read weightmap: %s"), *Message.ToString()));
		}

		if (ImportDescriptor.ImportResolutions.Num() == 0)
		{
			return MakeErrorResult(TEXT("Weightmap import descriptor has no valid resolutions"));
		}

		const int32 ImportWidth = ImportDescriptor.ImportResolutions[0].Width;
		const int32 ImportHeight = ImportDescriptor.ImportResolutions[0].Height;

		// Load weightmap data
		TArray<uint8> WeightData;
		ELandscapeImportResult DataResult = FLandscapeImportHelper::GetWeightmapImportData(
			ImportDescriptor, 0 /*DescriptorIndex*/, FName(*LayerName), WeightData, Message);

		if (DataResult == ELandscapeImportResult::Error)
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to load weightmap data: %s"), *Message.ToString()));
		}

		// Write weightmap data
		int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
		LandscapeInfo->GetLandscapeExtent(MinX, MinY, MaxX, MaxY);

		FLandscapeEditDataInterface EditInterface(LandscapeInfo);
		EditInterface.SetAlphaData(
			TargetLayerInfo,
			MinX, MinY, MaxX, MaxY,
			WeightData.GetData(), 0 /*Stride*/,
			ELandscapeLayerPaintingRestriction::None,
			true /*bWeightAdjust*/);

		// Build result
		TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetStringField(TEXT("type"), TEXT("weightmap"));
		Data->SetStringField(TEXT("file_path"), FilePath);

		TSharedPtr<FJsonObject> Resolution = MakeShared<FJsonObject>();
		Resolution->SetNumberField(TEXT("width"), ImportWidth);
		Resolution->SetNumberField(TEXT("height"), ImportHeight);
		Data->SetObjectField(TEXT("resolution"), Resolution);

		Data->SetStringField(TEXT("layer_name"), LayerName);
		Data->SetStringField(TEXT("landscape_name"), Proxy->GetActorLabel());
		Data->SetStringField(TEXT("landscape_path"), Proxy->GetPathName());

		FString Summary = FString::Printf(
			TEXT("Imported weightmap (%dx%d) from %s into %s, layer '%s'"),
			ImportWidth, ImportHeight, *FilePath, *Proxy->GetActorLabel(), *LayerName);

		FToolResult Result = MakeSuccessResult(Data, Summary);
		if (DescResult == ELandscapeImportResult::Warning)
		{
			Result.Warnings.Add(Message.ToString());
		}
		return Result;
	}

	return MakeErrorResult(FString::Printf(TEXT("Unknown import type: '%s'. Use 'heightmap' or 'weightmap'."), *ImportType));
}
