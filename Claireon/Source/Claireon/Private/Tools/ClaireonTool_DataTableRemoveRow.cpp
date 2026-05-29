// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableRemoveRow.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonScopedAssetLock.h"
#include "Engine/DataTable.h"
#include "DataTableEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"

FString ClaireonTool_DataTableRemoveRow::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableRemoveRow::GetOperation() const { return TEXT("remove_row"); }

FString ClaireonTool_DataTableRemoveRow::GetDescription() const
{
    return TEXT("Remove a data table row by name. Stateless / non-session: writes the asset directly by path without opening any editing session.");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableRemoveRow::GetInputSchema() const
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
	RowNameProp->SetStringField(TEXT("description"), TEXT("Name of the row to remove"));
	Properties->SetObjectField(TEXT("row_name"), RowNameProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("row_name")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableRemoveRow::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	if (DataTable->FindRowUnchecked(RowName) == nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("Row '%s' not found in %s"), *RowNameStr, *AssetPath));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] DataTable Remove Row")));
	FDataTableEditorUtils::RemoveRow(DataTable, RowName);

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
	Data->SetBoolField(TEXT("removed"), true);
	Data->SetNumberField(TEXT("remaining_rows"), RemainingRows);

	const FString Summary = FString::Printf(TEXT("Removed row '%s' from %s"), *RowNameStr, *TableName);

	return MakeSuccessResult(Data, Summary);
}
