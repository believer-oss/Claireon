// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LandscapeImport.h"
#include "Tools/ClaireonLandscapeTool_ImportHeightmap.h"
#include "Tools/ClaireonLandscapeTool_ImportWeightmap.h"
#include "ClaireonLog.h"

FString ClaireonTool_LandscapeImport::GetCategory() const { return TEXT("landscape"); }
FString ClaireonTool_LandscapeImport::GetOperation() const { return TEXT("import"); }

FString ClaireonTool_LandscapeImport::GetDescription() const
{
	return TEXT("DEPRECATED: dispatches on the 'type' enum. Use the per-type tools instead: "
	            "landscape_import_heightmap, landscape_import_weightmap. Stateless / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_LandscapeImport::GetInputSchema() const
{
	// Preserved schema for backwards-compatible callers; new callers should target the per-type tools directly.
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

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

	TSharedPtr<FJsonObject> FileProp = MakeShared<FJsonObject>();
	FileProp->SetStringField(TEXT("type"), TEXT("string"));
	FileProp->SetStringField(TEXT("description"), TEXT("Absolute path to the file on disk"));
	Properties->SetObjectField(TEXT("file_path"), FileProp);

	TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"), TEXT("Name or partial match of target landscape. Required for weightmap or importing into existing landscape."));
	Properties->SetObjectField(TEXT("landscape_name"), NameProp);

	TSharedPtr<FJsonObject> LayerProp = MakeShared<FJsonObject>();
	LayerProp->SetStringField(TEXT("type"), TEXT("string"));
	LayerProp->SetStringField(TEXT("description"), TEXT("Target weight layer name. Required when type is 'weightmap'."));
	Properties->SetObjectField(TEXT("layer_name"), LayerProp);

	TSharedPtr<FJsonObject> CreateProp = MakeShared<FJsonObject>();
	CreateProp->SetStringField(TEXT("type"), TEXT("boolean"));
	CreateProp->SetStringField(TEXT("description"), TEXT("If true and type is 'heightmap', create a new landscape from the imported data. Default: false."));
	Properties->SetObjectField(TEXT("create_landscape"), CreateProp);

	TSharedPtr<FJsonObject> LocationProp = MakeShared<FJsonObject>();
	LocationProp->SetStringField(TEXT("type"), TEXT("object"));
	LocationProp->SetStringField(TEXT("description"), TEXT("World location {x, y, z} for new landscape. Only used with create_landscape."));
	Properties->SetObjectField(TEXT("location"), LocationProp);

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

IClaireonTool::FToolResult ClaireonTool_LandscapeImport::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString ImportType;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("type"), ImportType) || ImportType.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: type ('heightmap' or 'weightmap'). DEPRECATED: prefer landscape_import_heightmap / landscape_import_weightmap."));
	}

	if (ImportType == TEXT("heightmap"))
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[landscape_import] DEPRECATED: forward this call to 'landscape_import_heightmap' (per-type tool). "
			     "The dispatcher will be removed in a future release."));
		ClaireonLandscapeTool_ImportHeightmap Tool;
		return Tool.Execute(Arguments);
	}
	if (ImportType == TEXT("weightmap"))
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("[landscape_import] DEPRECATED: forward this call to 'landscape_import_weightmap' (per-type tool). "
			     "The dispatcher will be removed in a future release."));
		ClaireonLandscapeTool_ImportWeightmap Tool;
		return Tool.Execute(Arguments);
	}

	return MakeErrorResult(FString::Printf(TEXT("Unknown import type: '%s'. Use 'heightmap' or 'weightmap'."), *ImportType));
}
