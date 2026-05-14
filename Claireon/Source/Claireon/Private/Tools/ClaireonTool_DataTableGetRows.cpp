// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableGetRows.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "Engine/DataTable.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_DataTableGetRows::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableGetRows::GetOperation() const { return TEXT("get_rows"); }

FString ClaireonTool_DataTableGetRows::GetDescription() const
{
	return TEXT("List row names with optional column value projection and pagination");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableGetRows::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Fully qualified asset path (e.g. /Game/Data/DT_Items)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// columns - optional array of strings
	TSharedPtr<FJsonObject> ColumnsProp = MakeShared<FJsonObject>();
	ColumnsProp->SetStringField(TEXT("type"), TEXT("array"));
	ColumnsProp->SetStringField(TEXT("description"), TEXT("Column names to include in output (omit for row names only)"));
	{
		TSharedPtr<FJsonObject> ItemsObj = MakeShared<FJsonObject>();
		ItemsObj->SetStringField(TEXT("type"), TEXT("string"));
		ColumnsProp->SetObjectField(TEXT("items"), ItemsObj);
	}
	Properties->SetObjectField(TEXT("columns"), ColumnsProp);

	// max_rows - optional integer
	TSharedPtr<FJsonObject> MaxRowsProp = MakeShared<FJsonObject>();
	MaxRowsProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxRowsProp->SetStringField(TEXT("description"), TEXT("Maximum number of rows to return (default: 100, max: 1000)"));
	Properties->SetObjectField(TEXT("max_rows"), MaxRowsProp);

	// offset - optional integer
	TSharedPtr<FJsonObject> OffsetProp = MakeShared<FJsonObject>();
	OffsetProp->SetStringField(TEXT("type"), TEXT("integer"));
	OffsetProp->SetStringField(TEXT("description"), TEXT("Number of rows to skip (default: 0)"));
	Properties->SetObjectField(TEXT("offset"), OffsetProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableGetRows::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString LoadError;
	UDataTable* DataTable = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, LoadError);
	if (!DataTable)
	{
		return MakeErrorResult(LoadError);
	}

	// Parse pagination parameters
	int32 MaxRows = 100;
	if (Arguments->HasField(TEXT("max_rows")))
	{
		MaxRows = FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("max_rows"))), 1, 1000);
	}

	int32 Offset = 0;
	if (Arguments->HasField(TEXT("offset")))
	{
		Offset = FMath::Max(0, static_cast<int32>(Arguments->GetNumberField(TEXT("offset"))));
	}

	// Parse optional column filter
	TArray<FString> RequestedColumns;
	if (Arguments->HasField(TEXT("columns")))
	{
		const TArray<TSharedPtr<FJsonValue>>& ColsArray = Arguments->GetArrayField(TEXT("columns"));
		for (const TSharedPtr<FJsonValue>& Val : ColsArray)
		{
			FString ColName;
			if (Val->TryGetString(ColName) && !ColName.IsEmpty())
			{
				RequestedColumns.Add(ColName);
			}
		}
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	const TMap<FName, uint8*>& RowMap = DataTable->GetRowMap();
	const int32 TotalRows = RowMap.Num();

	// Collect rows into ordered array
	TArray<FName> RowNames;
	RowNames.Reserve(TotalRows);
	for (const auto& Pair : RowMap)
	{
		RowNames.Add(Pair.Key);
	}

	// Apply offset and limit
	const int32 StartIndex = FMath::Min(Offset, TotalRows);
	const int32 EndIndex = FMath::Min(StartIndex + MaxRows, TotalRows);
	const int32 ReturnedRows = EndIndex - StartIndex;

	// Build rows array
	TArray<TSharedPtr<FJsonValue>> RowsArray;
	for (int32 i = StartIndex; i < EndIndex; ++i)
	{
		const FName& RowName = RowNames[i];
		const uint8* RowData = RowMap.FindChecked(RowName);

		TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
		RowObj->SetStringField(TEXT("row_name"), RowName.ToString());

		if (RowStruct && RowData)
		{
			for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
			{
				const FProperty* Prop = *It;
				const FString PropName = Prop->GetName();

				// Filter by requested columns if specified
				if (RequestedColumns.Num() > 0)
				{
					bool bWanted = false;
					for (const FString& Col : RequestedColumns)
					{
						if (Col.Equals(PropName, ESearchCase::IgnoreCase))
						{
							bWanted = true;
							break;
						}
					}
					if (!bWanted)
					{
						continue;
					}
				}

				RowObj->SetStringField(PropName, ClaireonDataTableHelpers::GetPropertyValueAsString(RowData, Prop));
			}
		}

		RowsArray.Add(MakeShared<FJsonValueObject>(RowObj));
	}

	const FString TableName = FPaths::GetBaseFilename(AssetPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), AssetPath);
	Data->SetArrayField(TEXT("rows"), RowsArray);
	Data->SetNumberField(TEXT("total_rows"), TotalRows);
	Data->SetNumberField(TEXT("returned_rows"), ReturnedRows);

	const FString Summary = FString::Printf(TEXT("%s: showing rows %d-%d of %d"),
		*TableName, StartIndex + 1, EndIndex, TotalRows);

	return MakeSuccessResult(Data, Summary);
}
