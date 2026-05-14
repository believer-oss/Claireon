// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LandscapeInspect.h"
#include "Tools/ClaireonLandscapeHelpers.h"
#include "ClaireonLog.h"
#include "Editor.h"
#include "LandscapeInfo.h"
#include "LandscapeProxy.h"

FString ClaireonTool_LandscapeInspect::GetCategory() const { return TEXT("landscape"); }
FString ClaireonTool_LandscapeInspect::GetOperation() const { return TEXT("inspect"); }

FString ClaireonTool_LandscapeInspect::GetDescription() const
{
	return TEXT("Inspect landscape metadata including dimensions, materials, weight layers, and splines.");
}

TSharedPtr<FJsonObject> ClaireonTool_LandscapeInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// landscape_name - optional
	TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"), TEXT("Name or partial match of landscape. If omitted, returns info for all landscapes."));
	Properties->SetObjectField(TEXT("landscape_name"), NameProp);

	// detail_level - optional
	TSharedPtr<FJsonObject> DetailProp = MakeShared<FJsonObject>();
	DetailProp->SetStringField(TEXT("type"), TEXT("string"));
	DetailProp->SetStringField(TEXT("description"), TEXT("'summary' or 'full'. Summary omits weight layers, splines, material details. Default: full."));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("full")));
		DetailProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("detail_level"), DetailProp);

	Schema->SetObjectField(TEXT("properties"), Properties);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_LandscapeInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	// Extract optional parameters
	FString LandscapeName;
	Arguments->TryGetStringField(TEXT("landscape_name"), LandscapeName);

	FString DetailLevel;
	if (!Arguments->TryGetStringField(TEXT("detail_level"), DetailLevel) || DetailLevel.IsEmpty())
	{
		DetailLevel = TEXT("full");
	}

	// Find landscapes
	TArray<TPair<ULandscapeInfo*, ALandscapeProxy*>> Landscapes =
		ClaireonLandscapeHelpers::FindLandscapeInWorld(World, LandscapeName);

	if (Landscapes.Num() == 0)
	{
		if (!LandscapeName.IsEmpty())
		{
			return MakeErrorResult(FString::Printf(
				TEXT("No landscape matching '%s' found in the current world"), *LandscapeName));
		}
		return MakeErrorResult(TEXT("No landscapes found in the current world"));
	}

	// Build JSON array of landscape info
	TArray<TSharedPtr<FJsonValue>> LandscapeArray;
	TArray<FString> SummaryParts;

	for (const TPair<ULandscapeInfo*, ALandscapeProxy*>& Pair : Landscapes)
	{
		TSharedPtr<FJsonObject> InfoJson = ClaireonLandscapeHelpers::BuildLandscapeInfoJson(Pair.Key, DetailLevel);
		if (!InfoJson)
		{
			UE_LOG(LogClaireon, Warning, TEXT("LandscapeInspect: BuildLandscapeInfoJson returned nullptr for '%s'"),
				*Pair.Value->GetActorLabel());
			continue;
		}

		LandscapeArray.Add(MakeShared<FJsonValueObject>(InfoJson));

		// Build summary part
		const FString Name = Pair.Value->GetActorLabel();
		int32 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;
		Pair.Key->GetLandscapeExtent(MinX, MinY, MaxX, MaxY);
		const int32 Width = MaxX - MinX;
		const int32 Height = MaxY - MinY;
		const int32 ComponentCount = Pair.Key->XYtoComponentMap.Num();
		const int32 LayerCount = Pair.Key->Layers.Num();

		SummaryParts.Add(FString::Printf(
			TEXT("%s (%dx%d, %d components, %d weight layers)"),
			*Name, Width, Height, ComponentCount, LayerCount));
	}

	// Build output
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("landscapes"), LandscapeArray);

	FString Summary = FString::Printf(
		TEXT("Found %d landscape(s): %s"),
		LandscapeArray.Num(),
		*FString::Join(SummaryParts, TEXT(", ")));

	return MakeSuccessResult(Data, Summary);
}
