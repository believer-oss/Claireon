// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableAddRow.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonScopedAssetLock.h"
#include "Engine/DataTable.h"
#include "DataTableEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"

FString ClaireonTool_DataTableAddRow::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableAddRow::GetOperation() const { return TEXT("add_row"); }

FString ClaireonTool_DataTableAddRow::GetDescription() const
{
    return TEXT("Add a new row to a data table at the given path. Stateless / non-session: writes the asset directly by path without opening any editing session.");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableAddRow::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Asset path to the data table (e.g. /Game/Data/DT_MyTable)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// row_name - required
	TSharedPtr<FJsonObject> RowNameProp = MakeShared<FJsonObject>();
	RowNameProp->SetStringField(TEXT("type"), TEXT("string"));
	RowNameProp->SetStringField(TEXT("description"), TEXT("Name for the new row (must be a valid FName)"));
	Properties->SetObjectField(TEXT("row_name"), RowNameProp);

	// values - optional
	TSharedPtr<FJsonObject> ValuesProp = MakeShared<FJsonObject>();
	ValuesProp->SetStringField(TEXT("type"), TEXT("object"));
	ValuesProp->SetStringField(TEXT("description"), TEXT("Property name to value pairs (ImportText format) to set on the new row"));
	Properties->SetObjectField(TEXT("values"), ValuesProp);

	// allow_overwrite - optional
	TSharedPtr<FJsonObject> AllowOverwriteProp = MakeShared<FJsonObject>();
	AllowOverwriteProp->SetStringField(TEXT("type"), TEXT("boolean"));
	AllowOverwriteProp->SetStringField(TEXT("description"), TEXT("If true, replace an existing row with the same name (default: false)"));
	Properties->SetObjectField(TEXT("allow_overwrite"), AllowOverwriteProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("row_name")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableAddRow::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FClaireonScopedAssetLock Lock(AssetPath, GetName());
	if (!Lock.IsAcquired())
	{
		return Lock.GetError();
	}

	FString RowNameStr;
	if (!Arguments->TryGetStringField(TEXT("row_name"), RowNameStr) || RowNameStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: row_name"));
	}

	bool bAllowOverwrite = false;
	if (Arguments->HasField(TEXT("allow_overwrite")))
	{
		bAllowOverwrite = Arguments->GetBoolField(TEXT("allow_overwrite"));
	}

	FString ValidateError;
	if (!ClaireonDataTableHelpers::ValidateRowName(RowNameStr, ValidateError))
	{
		return MakeErrorResult(ValidateError);
	}

	FString LoadError;
	UDataTable* DataTable = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, LoadError);
	if (!DataTable)
	{
		return MakeErrorResult(LoadError);
	}

	FString WritableError;
	if (!ClaireonDataTableHelpers::EnsureWritable(DataTable, WritableError))
	{
		return MakeErrorResult(WritableError);
	}

	const FName RowName(*RowNameStr);
	if (DataTable->FindRowUnchecked(RowName) != nullptr && !bAllowOverwrite)
	{
		return MakeErrorResult(FString::Printf(TEXT("Row '%s' already exists. Use allow_overwrite=true to replace it."), *RowNameStr));
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (!RowStruct)
	{
		return MakeErrorResult(TEXT("DataTable has no row struct"));
	}

	// Add the row using the editor utility (handles Modify, broadcast, etc.)
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] DataTable Add Row")));
	FDataTableEditorUtils::AddRow(DataTable, RowName);

	// Apply initial values if provided
	int32 FieldCount = 0;
	if (Arguments->HasField(TEXT("values")))
	{
		const TSharedPtr<FJsonObject>* ValuesPtr = nullptr;
		if (Arguments->TryGetObjectField(TEXT("values"), ValuesPtr) && ValuesPtr && (*ValuesPtr).IsValid())
		{
			FString SetError;
			if (!ClaireonDataTableHelpers::SetPropertyValues(DataTable, RowName, *ValuesPtr, SetError))
			{
				return MakeErrorResult(FString::Printf(TEXT("Row added but failed to set values: %s"), *SetError));
			}
			FieldCount = (*ValuesPtr)->Values.Num();
		}
	}

	// Save
	FString SaveError;
	if (!ClaireonDataTableHelpers::SaveDataTable(DataTable, SaveError))
	{
		return MakeErrorResult(SaveError);
	}

	const FString TableName = FPaths::GetBaseFilename(AssetPath);
	const int32 RemainingRows = DataTable->GetRowMap().Num();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), AssetPath);
	Data->SetStringField(TEXT("row_name"), RowNameStr);
	Data->SetBoolField(TEXT("created"), true);
	Data->SetNumberField(TEXT("field_count"), FieldCount);

	const FString Summary = FString::Printf(TEXT("Added row '%s' to %s"), *RowNameStr, *TableName);

	return MakeSuccessResult(Data, Summary);
}
