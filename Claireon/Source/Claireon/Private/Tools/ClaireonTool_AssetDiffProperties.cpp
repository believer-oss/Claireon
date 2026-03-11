// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetDiffProperties.h"

#include "ClaireonLog.h"
#include "Tools/ClaireonDiffHelpers.h"
#include "DiffUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_AssetDiffProperties::GetName() const
{
	return TEXT("diff_assets");
}

FString ClaireonTool_AssetDiffProperties::GetCategory() const
{
	return TEXT("diff");
}

FString ClaireonTool_AssetDiffProperties::GetDescription() const
{
	return TEXT("Compare properties between any two UObjects. Supports loading assets from the current editor state "
		"or from git revisions. Each side is an asset path with an optional git revision. "
		"At least one revision must be specified, or asset_path_b must differ from asset_path_a. "
		"Use resolution='exists' for a quick boolean check, 'summary' for a list of differences, "
		"or 'detailed' for full old/new values.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetDiffProperties::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path_a (required)
	TSharedPtr<FJsonObject> PathAProp = MakeShared<FJsonObject>();
	PathAProp->SetStringField(TEXT("type"), TEXT("string"));
	PathAProp->SetStringField(TEXT("description"), TEXT("Unreal content path for side A (e.g. /Game/Data/DA_Config)"));
	Properties->SetObjectField(TEXT("asset_path_a"), PathAProp);

	// revision_a (optional)
	TSharedPtr<FJsonObject> RevAProp = MakeShared<FJsonObject>();
	RevAProp->SetStringField(TEXT("type"), TEXT("string"));
	RevAProp->SetStringField(TEXT("description"), TEXT("Git revision for side A (e.g. HEAD, HEAD~1, abc123). If omitted, loads from current editor state."));
	Properties->SetObjectField(TEXT("revision_a"), RevAProp);

	// asset_path_b (optional)
	TSharedPtr<FJsonObject> PathBProp = MakeShared<FJsonObject>();
	PathBProp->SetStringField(TEXT("type"), TEXT("string"));
	PathBProp->SetStringField(TEXT("description"), TEXT("Unreal content path for side B. Defaults to asset_path_a if omitted."));
	Properties->SetObjectField(TEXT("asset_path_b"), PathBProp);

	// revision_b (optional)
	TSharedPtr<FJsonObject> RevBProp = MakeShared<FJsonObject>();
	RevBProp->SetStringField(TEXT("type"), TEXT("string"));
	RevBProp->SetStringField(TEXT("description"), TEXT("Git revision for side B. If omitted, loads from current editor state."));
	Properties->SetObjectField(TEXT("revision_b"), RevBProp);

	// resolution (optional)
	TSharedPtr<FJsonObject> ResProp = MakeShared<FJsonObject>();
	ResProp->SetStringField(TEXT("type"), TEXT("string"));
	ResProp->SetStringField(TEXT("description"), TEXT("Output detail level: 'exists' (boolean), 'summary' (list diffs), 'detailed' (with values). Default: summary."));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("exists")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("summary")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("detailed")));
		ResProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("resolution"), ResProp);

	// property_filter (optional)
	TSharedPtr<FJsonObject> FilterProp = MakeShared<FJsonObject>();
	FilterProp->SetStringField(TEXT("type"), TEXT("array"));
	FilterProp->SetStringField(TEXT("description"), TEXT("Optional list of property names to include. Only matching properties will be shown."));
	{
		TSharedPtr<FJsonObject> ItemSchema = MakeShared<FJsonObject>();
		ItemSchema->SetStringField(TEXT("type"), TEXT("string"));
		FilterProp->SetObjectField(TEXT("items"), ItemSchema);
	}
	Properties->SetObjectField(TEXT("property_filter"), FilterProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path_a")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AssetDiffProperties::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Parse required path_a
	FString PathA;
	if (!Arguments->TryGetStringField(TEXT("asset_path_a"), PathA) || PathA.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: asset_path_a"));
	}

	// Validate path
	FString ValidationError;
	if (!ClaireonDiffHelpers::ValidateAssetPath(PathA, ValidationError))
	{
		return MakeErrorResult(ValidationError);
	}

	FString PathB = PathA;
	Arguments->TryGetStringField(TEXT("asset_path_b"), PathB);

	FString RevisionA;
	FString RevisionB;
	Arguments->TryGetStringField(TEXT("revision_a"), RevisionA);
	Arguments->TryGetStringField(TEXT("revision_b"), RevisionB);

	// Validate parameters
	FString ParamError;
	if (!ClaireonDiffHelpers::ValidateDiffParameters(PathA, RevisionA, PathB, RevisionB, ParamError))
	{
		return MakeErrorResult(ParamError);
	}

	// Resolution
	FString ResolutionStr = TEXT("summary");
	Arguments->TryGetStringField(TEXT("resolution"), ResolutionStr);
	ClaireonDiffHelpers::EDiffResolution Resolution;
	FString ResError;
	if (!ClaireonDiffHelpers::ParseResolution(ResolutionStr, Resolution, ResError))
	{
		return MakeErrorResult(ResError);
	}

	// Optional property filter
	TSet<FString> PropertyFilter;
	const TArray<TSharedPtr<FJsonValue>>* FilterArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("property_filter"), FilterArray) && FilterArray)
	{
		for (const TSharedPtr<FJsonValue>& Val : *FilterArray)
		{
			FString PropName;
			if (Val->TryGetString(PropName))
			{
				PropertyFilter.Add(PropName);
			}
		}
	}

	// Resolve both sides
	ClaireonDiffHelpers::FResolvedDiffSide SideA = ClaireonDiffHelpers::ResolveDiffSide(PathA, RevisionA, ValidationError);
	if (!SideA.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to resolve side A: %s"), *ValidationError));
	}
	ClaireonDiffHelpers::FScopedTempFile TempA(SideA.TempFilePath);

	ClaireonDiffHelpers::FResolvedDiffSide SideB = ClaireonDiffHelpers::ResolveDiffSide(PathB, RevisionB, ValidationError);
	if (!SideB.IsValid())
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to resolve side B: %s"), *ValidationError));
	}
	ClaireonDiffHelpers::FScopedTempFile TempB(SideB.TempFilePath);

	// Compare using DiffUtils
	TArray<FSingleObjectDiffEntry> DiffEntries;
	DiffUtils::CompareUnrelatedObjects(SideA.Object, SideB.Object, DiffEntries);

	// Build structured result
	TArray<TSharedPtr<FJsonValue>> AddedArray;
	TArray<TSharedPtr<FJsonValue>> RemovedArray;
	TArray<TSharedPtr<FJsonValue>> ChangedArray;

	for (const FSingleObjectDiffEntry& Entry : DiffEntries)
	{
		const FString PropName = Entry.Identifier.ToDisplayName();

		// Apply property filter
		if (!PropertyFilter.IsEmpty() && !PropertyFilter.Contains(PropName))
		{
			continue;
		}

		if (Resolution == ClaireonDiffHelpers::EDiffResolution::Exists)
		{
			// Just report existence immediately
			TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
			Data->SetStringField(TEXT("path_a"), PathA);
			Data->SetStringField(TEXT("path_b"), PathB);
			Data->SetBoolField(TEXT("has_differences"), true);
			Data->SetArrayField(TEXT("added"), AddedArray);
			Data->SetArrayField(TEXT("removed"), RemovedArray);
			Data->SetArrayField(TEXT("changed"), ChangedArray);
			return MakeSuccessResult(Data, TEXT("Assets differ"));
		}

		if (Entry.DiffType == EPropertyDiffType::PropertyValueChanged)
		{
			TSharedPtr<FJsonObject> ChangeObj = MakeShared<FJsonObject>();
			ChangeObj->SetStringField(TEXT("name"), PropName);

			if (Resolution == ClaireonDiffHelpers::EDiffResolution::Detailed)
			{
				// Get old/new values
				FString OldVal;
				FString NewVal;
				if (SideA.Object && SideB.Object)
				{
					UClass* ClassA = SideA.Object->GetClass();
					if (FProperty* Prop = ClassA->FindPropertyByName(FName(*PropName)))
					{
						OldVal = ClaireonDiffHelpers::ExportPropertyValue(Prop, Prop->ContainerPtrToValuePtr<void>(SideA.Object));
						NewVal = ClaireonDiffHelpers::ExportPropertyValue(Prop, Prop->ContainerPtrToValuePtr<void>(SideB.Object));
					}
				}
				ChangeObj->SetStringField(TEXT("old_value"), OldVal);
				ChangeObj->SetStringField(TEXT("new_value"), NewVal);
			}
			ChangedArray.Add(MakeShared<FJsonValueObject>(ChangeObj));
		}
		else if (Entry.DiffType == EPropertyDiffType::PropertyAddedToA)
		{
			RemovedArray.Add(MakeShared<FJsonValueString>(PropName));
		}
		else if (Entry.DiffType == EPropertyDiffType::PropertyAddedToB)
		{
			AddedArray.Add(MakeShared<FJsonValueString>(PropName));
		}
	}

	const int32 TotalDiffs = AddedArray.Num() + RemovedArray.Num() + ChangedArray.Num();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("path_a"), PathA);
	Data->SetStringField(TEXT("path_b"), PathB);
	Data->SetArrayField(TEXT("added"), AddedArray);
	Data->SetArrayField(TEXT("removed"), RemovedArray);
	Data->SetArrayField(TEXT("changed"), ChangedArray);

	const FString Summary = FString::Printf(TEXT("%d properties differ between A and B"), TotalDiffs);
	return MakeSuccessResult(Data, Summary);
}
