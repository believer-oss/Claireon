// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeTool_ImportWeightmap.h"
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
#include "Misc/EngineVersionComparison.h"

FString ClaireonLandscapeTool_ImportWeightmap::GetCategory() const { return TEXT("landscape"); }
FString ClaireonLandscapeTool_ImportWeightmap::GetOperation() const { return TEXT("import_weightmap"); }

FString ClaireonLandscapeTool_ImportWeightmap::GetDescription() const
{
	return TEXT("Import a weightmap (R16, PNG, RAW) into a specific layer of an existing landscape actor. The landscape must already exist with the named layer. Stateless / non-session.");
}

TSharedPtr<FJsonObject> ClaireonLandscapeTool_ImportWeightmap::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> FileProp = MakeShared<FJsonObject>();
	FileProp->SetStringField(TEXT("type"), TEXT("string"));
	FileProp->SetStringField(TEXT("description"), TEXT("Absolute path to the weightmap file on disk (.r16, .raw, .png)."));
	Properties->SetObjectField(TEXT("file_path"), FileProp);

	TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"), TEXT("Name or partial match of target landscape."));
	Properties->SetObjectField(TEXT("landscape_name"), NameProp);

	TSharedPtr<FJsonObject> LayerProp = MakeShared<FJsonObject>();
	LayerProp->SetStringField(TEXT("type"), TEXT("string"));
	LayerProp->SetStringField(TEXT("description"), TEXT("Target weight-layer name (must already exist on the landscape)."));
	Properties->SetObjectField(TEXT("layer_name"), LayerProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("file_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("landscape_name")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("layer_name")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonLandscapeTool_ImportWeightmap::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	if (!Arguments->TryGetStringField(TEXT("landscape_name"), LandscapeName) || LandscapeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: landscape_name"));
	}

	FString LayerName;
	if (!Arguments->TryGetStringField(TEXT("layer_name"), LayerName) || LayerName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: layer_name"));
	}

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

	TArray<uint8> WeightData;
	ELandscapeImportResult DataResult = FLandscapeImportHelper::GetWeightmapImportData(
		ImportDescriptor, 0 /*DescriptorIndex*/, FName(*LayerName), WeightData, Message);

	if (DataResult == ELandscapeImportResult::Error)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to load weightmap data: %s"), *Message.ToString()));
	}

	int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
	LandscapeInfo->GetLandscapeExtent(MinX, MinY, MaxX, MaxY);

	FLandscapeEditDataInterface EditInterface(LandscapeInfo);
#if UE_VERSION_OLDER_THAN(5, 7, 0)
	EditInterface.SetAlphaData(
		TargetLayerInfo,
		MinX, MinY, MaxX, MaxY,
		WeightData.GetData(), 0 /*Stride*/,
		ELandscapeLayerPaintingRestriction::None,
		true /*bWeightAdjust*/);
#else
	EditInterface.SetAlphaData(
		TargetLayerInfo,
		MinX, MinY, MaxX, MaxY,
		WeightData.GetData(), 0 /*Stride*/,
		ELandscapeLayerPaintingRestriction::None);
#endif

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
