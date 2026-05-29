// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableGetRowJson.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "Engine/DataTable.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "JsonObjectConverter.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FString ClaireonTool_DataTableGetRowJson::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableGetRowJson::GetOperation() const { return TEXT("get_row_json"); }

FString ClaireonTool_DataTableGetRowJson::GetDescription() const
{
	return TEXT(
		"Get a single DataTable row as JSON using the same FJsonObjectConverter encoding "
		"as datatable_export_json -- without loading or exporting the entire table. "
		"Output is round-trippable with datatable_import_json. "
		"Prefer datatable_get_row when you need friendly property names, "
		"BP GUID-suffix stripping, or FText / TMap expansion. "
		"Read-only / non-session."
	);
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableGetRowJson::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Fully qualified asset path (e.g. /Game/Data/DT_Items)"));
		Properties->SetObjectField(TEXT("asset_path"), Prop);
	}
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Name of the row to retrieve"));
		Properties->SetObjectField(TEXT("row_name"), Prop);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("row_name")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableGetRowJson::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	UScriptStruct* RowStruct = const_cast<UScriptStruct*>(DataTable->GetRowStruct());
	if (!RowStruct)
	{
		return MakeErrorResult(FString::Printf(TEXT("DataTable '%s' has no row struct"), *AssetPath));
	}

	TSharedRef<FJsonObject> RowJson = MakeShared<FJsonObject>();
	if (!FJsonObjectConverter::UStructToJsonObject(RowStruct, RowData, RowJson, /*CheckFlags=*/0, /*SkipFlags=*/0))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to serialize row '%s' as JSON"), *RowNameStr));
	}

	const FString TableName = FPaths::GetBaseFilename(AssetPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), AssetPath);
	Data->SetStringField(TEXT("row_name"), RowNameStr);
	Data->SetObjectField(TEXT("row"), RowJson);

	const FString Summary = FString::Printf(TEXT("Row '%s' from %s (native JSON)"), *RowNameStr, *TableName);
	return MakeSuccessResult(Data, Summary);
}
