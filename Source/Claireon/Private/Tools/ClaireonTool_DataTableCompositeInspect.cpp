// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableCompositeInspect.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "Engine/DataTable.h"
#include "Engine/CompositeDataTable.h"
#include "Misc/PackageName.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// Resolves a loaded DataTable's package to its on-disk timestamp. MinValue() (file missing/unsaved) -> unset.
static bool GetDataTableLastSaved(const UDataTable* DataTable, FDateTime& OutTimestamp)
{
	if (!IsValid(DataTable))
	{
		return false;
	}
	FString Filename;
	if (!FPackageName::TryConvertLongPackageNameToFilename(DataTable->GetPackage()->GetName(), Filename, FPackageName::GetAssetPackageExtension()))
	{
		return false;
	}
	OutTimestamp = IFileManager::Get().GetTimeStamp(*Filename);
	return OutTimestamp != FDateTime::MinValue();
}

FString ClaireonTool_DataTableCompositeInspect::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableCompositeInspect::GetOperation() const { return TEXT("composite_inspect"); }

FString ClaireonTool_DataTableCompositeInspect::GetDescription() const
{
	return TEXT("Inspect a composite data table: list its parents, staleness (per-parent timestamp versus the composite), row count, and per-row source-parent provenance. Read-only and non-session -- it acquires no lock and writes nothing, so no close call is required.");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableCompositeInspect::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Asset path to the composite data table (e.g. /Game/Data/DT_Composite_Items)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableCompositeInspect::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString LoadError;
	UDataTable* DataTable = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, LoadError);
	if (!IsValid(DataTable))
	{
		return MakeErrorResult(LoadError);
	}

	UCompositeDataTable* Composite = ClaireonDataTableHelpers::AsCompositeDataTable(DataTable);
	if (!IsValid(Composite))
	{
		return MakeErrorResult(TEXT("Asset is not a Composite Data Table; nothing to inspect."));
	}

	// Live parent objects (already resolved by the composite); derive paths from them, no re-load.
	const TArray<UDataTable*> ParentTables = ClaireonDataTableHelpers::GetCompositeParentTableObjects(Composite);
	TArray<FString> ParentPaths;
	ParentPaths.Reserve(ParentTables.Num());
	for (const UDataTable* Parent : ParentTables)
	{
		ParentPaths.Add(IsValid(Parent) ? Parent->GetPathName() : FString());
	}

	FDateTime CompositeTimestamp;
	const bool bCompositeHasTimestamp = GetDataTableLastSaved(Composite, CompositeTimestamp);

	TArray<TSharedPtr<FJsonValue>> ParentsArray;
	TArray<TSharedPtr<FJsonValue>> ParentDetailsArray;
	FString StaleParentName;
	FDateTime StaleParentTimestamp;
	bool bAnyStale = false;

	for (int32 i = 0; i < ParentTables.Num(); ++i)
	{
		const UDataTable* Parent = ParentTables[i];
		const FString& ParentPath = ParentPaths[i];
		ParentsArray.Add(MakeShared<FJsonValueString>(ParentPath));

		TSharedPtr<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("path"), ParentPath);

		FDateTime ParentTimestamp;
		const bool bParentHasTimestamp = GetDataTableLastSaved(Parent, ParentTimestamp);
		if (bParentHasTimestamp)
		{
			Detail->SetStringField(TEXT("last_saved"), ParentTimestamp.ToIso8601());
			if (bCompositeHasTimestamp && ParentTimestamp > CompositeTimestamp)
			{
				bAnyStale = true;
				if (StaleParentName.IsEmpty() || ParentTimestamp > StaleParentTimestamp)
				{
					StaleParentName = ParentPath;
					StaleParentTimestamp = ParentTimestamp;
				}
			}
		}
		else
		{
			Detail->SetField(TEXT("last_saved"), MakeShared<FJsonValueNull>());
		}

		// A2: parent rows whose RowStruct differs from the composite's are silently dropped by UpdateCachedRowMap.
		const bool bRowStructMismatch = IsValid(Parent) && Parent->GetRowStruct() != Composite->GetRowStruct();
		Detail->SetBoolField(TEXT("row_struct_mismatch"), bRowStructMismatch);

		ParentDetailsArray.Add(MakeShared<FJsonValueObject>(Detail));
	}

	TSharedPtr<FJsonValue> StaleWarningValue;
	if (bAnyStale)
	{
		const FString CompositeTsStr = bCompositeHasTimestamp ? CompositeTimestamp.ToIso8601() : TEXT("(never saved)");
		StaleWarningValue = MakeShared<FJsonValueString>(FString::Printf(
			TEXT("Parent '%s' (saved %s) is newer than composite (saved %s); refresh recommended."),
			*StaleParentName, *StaleParentTimestamp.ToIso8601(), *CompositeTsStr));
	}
	else
	{
		StaleWarningValue = MakeShared<FJsonValueNull>();
	}

	// Per-row provenance: highest-index parent containing the row name wins (composite override order).
	const TArray<FName> RowNames = Composite->GetRowNames();
	const int32 RowCount = RowNames.Num();
	static constexpr int32 ROW_DETAIL_CAP = 2000;
	const int32 RowsToDetail = FMath::Min(RowCount, ROW_DETAIL_CAP);

	TSharedPtr<FJsonObject> RowsObj = MakeShared<FJsonObject>();
	for (int32 RowIdx = 0; RowIdx < RowsToDetail; ++RowIdx)
	{
		const FName& RowName = RowNames[RowIdx];
		FString SourceParent;
		for (int32 ParentIdx = ParentTables.Num() - 1; ParentIdx >= 0; --ParentIdx)
		{
			const UDataTable* Parent = ParentTables[ParentIdx];
			if (IsValid(Parent) && Parent->GetRowMap().Contains(RowName))
			{
				SourceParent = ParentPaths[ParentIdx];
				break;
			}
		}

		TSharedPtr<FJsonObject> RowDetail = MakeShared<FJsonObject>();
		if (SourceParent.IsEmpty())
		{
			RowDetail->SetField(TEXT("source_parent"), MakeShared<FJsonValueNull>());
		}
		else
		{
			RowDetail->SetStringField(TEXT("source_parent"), SourceParent);
		}
		RowDetail->SetBoolField(TEXT("cached"), true);
		RowsObj->SetObjectField(RowName.ToString(), RowDetail);
	}

	const FString TableName = FPaths::GetBaseFilename(AssetPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("parents"), ParentsArray);
	if (bCompositeHasTimestamp)
	{
		Data->SetStringField(TEXT("last_saved"), CompositeTimestamp.ToIso8601());
	}
	else
	{
		Data->SetField(TEXT("last_saved"), MakeShared<FJsonValueNull>());
	}
	Data->SetArrayField(TEXT("parent_details"), ParentDetailsArray);
	Data->SetField(TEXT("stale_warning"), StaleWarningValue);
	Data->SetNumberField(TEXT("row_count"), RowCount);
	Data->SetObjectField(TEXT("rows"), RowsObj);

	FString Summary = FString::Printf(TEXT("%s: %d rows from %d parents"), *TableName, RowCount, ParentPaths.Num());
	if (RowCount > ROW_DETAIL_CAP)
	{
		Summary += FString::Printf(TEXT(" (row detail capped at %d)"), ROW_DETAIL_CAP);
	}
	if (bAnyStale)
	{
		Summary += FString::Printf(TEXT("; STALE: parent '%s' is newer than the composite"), *StaleParentName);
	}

	return MakeSuccessResult(Data, Summary);
}
