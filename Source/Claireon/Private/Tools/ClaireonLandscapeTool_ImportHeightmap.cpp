// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeTool_ImportHeightmap.h"
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

namespace ClaireonLandscapeImportHeightmapInternal
{
	// File-local helper: extract FVector from JSON {x, y, z} with defaults.
	static FVector ExtractVectorFromJson(const TSharedPtr<FJsonObject>& Obj, const FVector& Default)
	{
		if (!Obj) return Default;
		FVector Result = Default;
		double Val;
		if (Obj->TryGetNumberField(TEXT("x"), Val)) Result.X = Val;
		if (Obj->TryGetNumberField(TEXT("y"), Val)) Result.Y = Val;
		if (Obj->TryGetNumberField(TEXT("z"), Val)) Result.Z = Val;
		return Result;
	}
}

FString ClaireonLandscapeTool_ImportHeightmap::GetCategory() const { return TEXT("landscape"); }
FString ClaireonLandscapeTool_ImportHeightmap::GetOperation() const { return TEXT("import_heightmap"); }

FString ClaireonLandscapeTool_ImportHeightmap::GetDescription() const
{
	return TEXT("Import a heightmap (R16, PNG, RAW) into a landscape actor. Set create_landscape=true to spawn a new ALandscape from the file; otherwise the import targets an existing landscape named via landscape_name and the file resolution must match. Stateless / non-session.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeTool_ImportHeightmap::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> FileProp = MakeShared<FJsonObject>();
	FileProp->SetStringField(TEXT("type"), TEXT("string"));
	FileProp->SetStringField(TEXT("description"), TEXT("Absolute path to the heightmap file on disk (.r16, .raw, .png)."));
	Properties->SetObjectField(TEXT("file_path"), FileProp);

	TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"), TEXT("Name or partial match of target landscape. Required unless create_landscape is true."));
	Properties->SetObjectField(TEXT("landscape_name"), NameProp);

	TSharedPtr<FJsonObject> CreateProp = MakeShared<FJsonObject>();
	CreateProp->SetStringField(TEXT("type"), TEXT("boolean"));
	CreateProp->SetStringField(TEXT("description"), TEXT("If true, create a new landscape from the imported data. Default: false."));
	Properties->SetObjectField(TEXT("create_landscape"), CreateProp);

	TSharedPtr<FJsonObject> LocationProp = MakeShared<FJsonObject>();
	LocationProp->SetStringField(TEXT("type"), TEXT("object"));
	LocationProp->SetStringField(TEXT("description"), TEXT("World location {x, y, z} for the new landscape. Only used with create_landscape."));
	Properties->SetObjectField(TEXT("location"), LocationProp);

	TSharedPtr<FJsonObject> ScaleProp = MakeShared<FJsonObject>();
	ScaleProp->SetStringField(TEXT("type"), TEXT("object"));
	ScaleProp->SetStringField(TEXT("description"), TEXT("Scale {x, y, z} for the new landscape. Only used with create_landscape."));
	Properties->SetObjectField(TEXT("scale"), ScaleProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("file_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonLandscapeTool_ImportHeightmap::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString FilePath;
	if (!Arguments->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: file_path"));
	}

	FString LandscapeName;
	Arguments->TryGetStringField(TEXT("landscape_name"), LandscapeName);

	bool bCreateLandscape = false;
	Arguments->TryGetBoolField(TEXT("create_landscape"), bCreateLandscape);

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

	if (!bCreateLandscape && LandscapeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("landscape_name is required when importing into an existing landscape"));
	}

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

		const TSharedPtr<FJsonObject>* LocationObj = nullptr;
		Arguments->TryGetObjectField(TEXT("location"), LocationObj);
		const FVector Location = ClaireonLandscapeImportHeightmapInternal::ExtractVectorFromJson(LocationObj ? *LocationObj : nullptr, FVector::ZeroVector);

		const TSharedPtr<FJsonObject>* ScaleObj = nullptr;
		Arguments->TryGetObjectField(TEXT("scale"), ScaleObj);
		const FVector Scale = ClaireonLandscapeImportHeightmapInternal::ExtractVectorFromJson(ScaleObj ? *ScaleObj : nullptr, FVector(100.0, 100.0, 100.0));

		FActorSpawnParameters SpawnParams;
		SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		ALandscape* NewLandscape = World->SpawnActor<ALandscape>(SpawnParams);

		if (!NewLandscape)
		{
			return MakeErrorResult(TEXT("Failed to spawn ALandscape actor"));
		}

		NewLandscape->SetActorLocation(Location);
		NewLandscape->SetActorScale3D(Scale);

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

	FLandscapeEditDataInterface EditInterface(LandscapeInfo);
	EditInterface.SetHeightData(
		MinX, MinY, MaxX, MaxY,
		HeightData.GetData(), 0 /*Stride*/,
		true /*CalcNormals*/);

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
