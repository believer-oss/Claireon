// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableGetRow.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "Engine/DataTable.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_DataTableGetRow::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableGetRow::GetOperation() const { return TEXT("get_row"); }

FString ClaireonTool_DataTableGetRow::GetDescription() const
{
    return TEXT("Get all property values for a single data table row. Stateless / read-only / non-session: reads the asset by path without opening any editing session.");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableGetRow::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Fully qualified asset path (e.g. /Game/Data/DT_Items)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// row_name - required
	TSharedPtr<FJsonObject> RowNameProp = MakeShared<FJsonObject>();
	RowNameProp->SetStringField(TEXT("type"), TEXT("string"));
	RowNameProp->SetStringField(TEXT("description"), TEXT("Name of the row to retrieve"));
	Properties->SetObjectField(TEXT("row_name"), RowNameProp);

	// columns - optional array of strings
	TSharedPtr<FJsonObject> ColumnsProp = MakeShared<FJsonObject>();
	ColumnsProp->SetStringField(TEXT("type"), TEXT("array"));
	ColumnsProp->SetStringField(TEXT("description"), TEXT("Specific column names to return (omit for all columns)"));
	{
		TSharedPtr<FJsonObject> ItemsObj = MakeShared<FJsonObject>();
		ItemsObj->SetStringField(TEXT("type"), TEXT("string"));
		ColumnsProp->SetObjectField(TEXT("items"), ItemsObj);
	}
	Properties->SetObjectField(TEXT("columns"), ColumnsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("row_name")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableGetRow::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString RowNameStr;
	if (!Arguments->TryGetStringField(TEXT("row_name"), RowNameStr) || RowNameStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: row_name"));
	}

	FString LoadError;
	UDataTable* DataTable = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, LoadError);
	if (!DataTable)
	{
		return MakeErrorResult(LoadError);
	}

	const FName RowName(*RowNameStr);
	const uint8* RowData = DataTable->FindRowUnchecked(RowName);
	if (!RowData)
	{
		return MakeErrorResult(FString::Printf(TEXT("Row '%s' not found in %s"), *RowNameStr, *AssetPath));
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

	// Build values object
	TSharedPtr<FJsonObject> ValuesObj = MakeShared<FJsonObject>();
	int32 FieldCount = 0;

	if (RowStruct)
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

			ValuesObj->SetStringField(PropName, ClaireonDataTableHelpers::GetPropertyValueAsString(RowData, Prop));
			++FieldCount;
		}
	}

	const FString TableName = FPaths::GetBaseFilename(AssetPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), AssetPath);
	Data->SetStringField(TEXT("row_name"), RowNameStr);
	Data->SetObjectField(TEXT("values"), ValuesObj);

	const FString Summary = FString::Printf(TEXT("Row '%s' from %s: %d fields"),
		*RowNameStr, *TableName, FieldCount);

	return MakeSuccessResult(Data, Summary);
}
