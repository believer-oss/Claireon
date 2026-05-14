// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableFindRows.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "Engine/DataTable.h"
#include "Internationalization/Regex.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_DataTableFindRows::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableFindRows::GetOperation() const { return TEXT("find_rows"); }

FString ClaireonTool_DataTableFindRows::GetDescription() const
{
	return TEXT("Find rows where a column value matches a filter");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableFindRows::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Fully qualified asset path (e.g. /Game/Data/DT_Items)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// column - required
	TSharedPtr<FJsonObject> ColumnProp = MakeShared<FJsonObject>();
	ColumnProp->SetStringField(TEXT("type"), TEXT("string"));
	ColumnProp->SetStringField(TEXT("description"), TEXT("Column name to filter on"));
	Properties->SetObjectField(TEXT("column"), ColumnProp);

	// value - required
	TSharedPtr<FJsonObject> ValueProp = MakeShared<FJsonObject>();
	ValueProp->SetStringField(TEXT("type"), TEXT("string"));
	ValueProp->SetStringField(TEXT("description"), TEXT("Value to match against the column"));
	Properties->SetObjectField(TEXT("value"), ValueProp);

	// match_mode - optional enum
	TSharedPtr<FJsonObject> MatchModeProp = MakeShared<FJsonObject>();
	MatchModeProp->SetStringField(TEXT("type"), TEXT("string"));
	MatchModeProp->SetStringField(TEXT("description"), TEXT("How to match the value: exact, contains, starts_with, or regex (default: exact)"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("exact")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("contains")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("starts_with")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("regex")));
		MatchModeProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("match_mode"), MatchModeProp);

	// max_results - optional integer
	TSharedPtr<FJsonObject> MaxResultsProp = MakeShared<FJsonObject>();
	MaxResultsProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxResultsProp->SetStringField(TEXT("description"), TEXT("Maximum number of matching rows to return (default: 50, max: 500)"));
	Properties->SetObjectField(TEXT("max_results"), MaxResultsProp);

	// return_columns - optional array of strings
	TSharedPtr<FJsonObject> ReturnColumnsProp = MakeShared<FJsonObject>();
	ReturnColumnsProp->SetStringField(TEXT("type"), TEXT("array"));
	ReturnColumnsProp->SetStringField(TEXT("description"), TEXT("Additional columns to include in output alongside the filter column"));
	{
		TSharedPtr<FJsonObject> ItemsObj = MakeShared<FJsonObject>();
		ItemsObj->SetStringField(TEXT("type"), TEXT("string"));
		ReturnColumnsProp->SetObjectField(TEXT("items"), ItemsObj);
	}
	Properties->SetObjectField(TEXT("return_columns"), ReturnColumnsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("column")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("value")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableFindRows::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString FilterColumn;
	if (!Arguments->TryGetStringField(TEXT("column"), FilterColumn) || FilterColumn.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: column"));
	}

	FString FilterValue;
	if (!Arguments->TryGetStringField(TEXT("value"), FilterValue))
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	FString MatchMode = TEXT("exact");
	Arguments->TryGetStringField(TEXT("match_mode"), MatchMode);

	int32 MaxResults = 50;
	if (Arguments->HasField(TEXT("max_results")))
	{
		MaxResults = FMath::Clamp(static_cast<int32>(Arguments->GetNumberField(TEXT("max_results"))), 1, 500);
	}

	TArray<FString> ReturnColumns;
	if (Arguments->HasField(TEXT("return_columns")))
	{
		const TArray<TSharedPtr<FJsonValue>>& ColsArray = Arguments->GetArrayField(TEXT("return_columns"));
		for (const TSharedPtr<FJsonValue>& Val : ColsArray)
		{
			FString ColName;
			if (Val->TryGetString(ColName) && !ColName.IsEmpty())
			{
				ReturnColumns.Add(ColName);
			}
		}
	}

	FString LoadError;
	UDataTable* DataTable = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, LoadError);
	if (!DataTable)
	{
		return MakeErrorResult(LoadError);
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (!RowStruct)
	{
		return MakeErrorResult(TEXT("DataTable has no row struct"));
	}

	// Find the filter property
	const FProperty* FilterProp = RowStruct->FindPropertyByName(FName(*FilterColumn));
	if (!FilterProp)
	{
		return MakeErrorResult(FString::Printf(TEXT("Column '%s' not found in row struct"), *FilterColumn));
	}

	const TMap<FName, uint8*>& RowMap = DataTable->GetRowMap();
	const int32 TotalRows = RowMap.Num();

	// Compile optional regex
	TOptional<FRegexPattern> RegexPattern;
	if (MatchMode == TEXT("regex"))
	{
		RegexPattern = FRegexPattern(FilterValue);
	}

	// Scan rows
	TArray<TSharedPtr<FJsonValue>> MatchingRows;
	int32 MatchCount = 0;

	for (const auto& Pair : RowMap)
	{
		const FName& RowName = Pair.Key;
		const uint8* RowData = Pair.Value;

		const FString CellValue = ClaireonDataTableHelpers::GetPropertyValueAsString(RowData, FilterProp);

		bool bMatches = false;
		if (MatchMode == TEXT("exact"))
		{
			bMatches = CellValue.Equals(FilterValue);
		}
		else if (MatchMode == TEXT("contains"))
		{
			bMatches = CellValue.Contains(FilterValue, ESearchCase::IgnoreCase);
		}
		else if (MatchMode == TEXT("starts_with"))
		{
			bMatches = CellValue.StartsWith(FilterValue, ESearchCase::IgnoreCase);
		}
		else if (MatchMode == TEXT("regex") && RegexPattern.IsSet())
		{
			FRegexMatcher Matcher(RegexPattern.GetValue(), CellValue);
			bMatches = Matcher.FindNext();
		}

		if (!bMatches)
		{
			continue;
		}

		++MatchCount;

		if (MatchingRows.Num() < MaxResults)
		{
			TSharedPtr<FJsonObject> RowObj = MakeShared<FJsonObject>();
			RowObj->SetStringField(TEXT("row_name"), RowName.ToString());
			RowObj->SetStringField(FilterColumn, CellValue);

			// Include additional return columns
			for (const FString& ExtraCol : ReturnColumns)
			{
				if (ExtraCol.Equals(FilterColumn, ESearchCase::IgnoreCase))
				{
					continue; // Already included above
				}
				const FProperty* ExtraProp = RowStruct->FindPropertyByName(FName(*ExtraCol));
				if (ExtraProp)
				{
					RowObj->SetStringField(ExtraCol, ClaireonDataTableHelpers::GetPropertyValueAsString(RowData, ExtraProp));
				}
			}

			MatchingRows.Add(MakeShared<FJsonValueObject>(RowObj));
		}
	}

	const FString TableName = FPaths::GetBaseFilename(AssetPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), AssetPath);
	Data->SetArrayField(TEXT("matching_rows"), MatchingRows);
	Data->SetNumberField(TEXT("match_count"), MatchCount);
	Data->SetNumberField(TEXT("total_rows"), TotalRows);

	const FString Summary = FString::Printf(TEXT("Found %d of %d rows matching filter"), MatchCount, TotalRows);

	return MakeSuccessResult(Data, Summary);
}
