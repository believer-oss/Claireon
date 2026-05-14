// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableRenameRow.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonScopedAssetLock.h"
#include "Engine/DataTable.h"
#include "DataTableEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"

FString ClaireonTool_DataTableRenameRow::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableRenameRow::GetOperation() const { return TEXT("rename_row"); }

FString ClaireonTool_DataTableRenameRow::GetDescription() const
{
	return TEXT("Change a row's name");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableRenameRow::GetInputSchema() const
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
	RowNameProp->SetStringField(TEXT("description"), TEXT("Current name of the row to rename"));
	Properties->SetObjectField(TEXT("row_name"), RowNameProp);

	// new_name - required
	TSharedPtr<FJsonObject> NewNameProp = MakeShared<FJsonObject>();
	NewNameProp->SetStringField(TEXT("type"), TEXT("string"));
	NewNameProp->SetStringField(TEXT("description"), TEXT("New name for the row (must be a valid FName and not already exist)"));
	Properties->SetObjectField(TEXT("new_name"), NewNameProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("row_name")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("new_name")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableRenameRow::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// 1. Parse and validate arguments
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

	FString RowName;
	if (!Arguments->TryGetStringField(TEXT("row_name"), RowName) || RowName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: row_name"));
	}

	FString NewName;
	if (!Arguments->TryGetStringField(TEXT("new_name"), NewName) || NewName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: new_name"));
	}

	// 2. Validate new name
	FString ValidateError;
	if (!ClaireonDataTableHelpers::ValidateRowName(NewName, ValidateError))
	{
		return MakeErrorResult(ValidateError);
	}

	// 3. Load and validate table
	FString Error;
	UDataTable* Table = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, Error);
	if (!Table)
	{
		return MakeErrorResult(Error);
	}
	if (!ClaireonDataTableHelpers::EnsureWritable(Table, Error))
	{
		return MakeErrorResult(Error);
	}

	// 4. Check source row exists
	const FName RowFName(*RowName);
	if (Table->FindRowUnchecked(RowFName) == nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("Row '%s' not found in data table '%s'."), *RowName, *AssetPath));
	}

	// 5. Check new name doesn't already exist
	const FName NewFName(*NewName);
	if (Table->FindRowUnchecked(NewFName) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("Row '%s' already exists in data table '%s'. Choose a different name."), *NewName, *AssetPath));
	}

	// 6. Rename the row
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] DataTable Rename Row")));
	bool bRenamed = FDataTableEditorUtils::RenameRow(Table, RowFName, NewFName);
	if (!bRenamed)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to rename row '%s' to '%s' in data table '%s'."), *RowName, *NewName, *AssetPath));
	}

	// 7. Auto-save
	FString SaveError;
	bool bSaved = ClaireonDataTableHelpers::SaveDataTable(Table, SaveError);

	FString Output = FString::Printf(TEXT("Row '%s' renamed to '%s' in '%s'."), *RowName, *NewName, *AssetPath);
	if (!bSaved)
	{
		Output += FString::Printf(TEXT("\nWarning: %s"), *SaveError);
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.datatable.rename_row: %s"), *Output);
	return MakeSuccessResult(nullptr, Output);
}
