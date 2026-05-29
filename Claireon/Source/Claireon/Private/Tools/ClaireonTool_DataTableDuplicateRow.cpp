// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableDuplicateRow.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonScopedAssetLock.h"
#include "Engine/DataTable.h"
#include "DataTableEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"

FString ClaireonTool_DataTableDuplicateRow::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableDuplicateRow::GetOperation() const { return TEXT("duplicate_row"); }

FString ClaireonTool_DataTableDuplicateRow::GetDescription() const
{
    return TEXT("Copy an existing data table row to a new row name. Stateless / non-session: writes the asset directly by path without opening any editing session.");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableDuplicateRow::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Asset path to the data table (e.g. /Game/Data/DT_MyTable)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	// source_row - required
	TSharedPtr<FJsonObject> SourceRowProp = MakeShared<FJsonObject>();
	SourceRowProp->SetStringField(TEXT("type"), TEXT("string"));
	SourceRowProp->SetStringField(TEXT("description"), TEXT("Name of the row to copy"));
	Properties->SetObjectField(TEXT("source_row"), SourceRowProp);

	// new_row_name - required
	TSharedPtr<FJsonObject> NewRowNameProp = MakeShared<FJsonObject>();
	NewRowNameProp->SetStringField(TEXT("type"), TEXT("string"));
	NewRowNameProp->SetStringField(TEXT("description"), TEXT("Name for the duplicated row (must be a valid FName and not already exist)"));
	Properties->SetObjectField(TEXT("new_row_name"), NewRowNameProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("source_row")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("new_row_name")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableDuplicateRow::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString SourceRow;
	if (!Arguments->TryGetStringField(TEXT("source_row"), SourceRow) || SourceRow.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: source_row"));
	}

	FString NewRowName;
	if (!Arguments->TryGetStringField(TEXT("new_row_name"), NewRowName) || NewRowName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: new_row_name"));
	}

	// 2. Validate new row name
	FString ValidateError;
	if (!ClaireonDataTableHelpers::ValidateRowName(NewRowName, ValidateError))
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
	const FName SourceFName(*SourceRow);
	if (Table->FindRowUnchecked(SourceFName) == nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("Source row '%s' not found in data table '%s'."), *SourceRow, *AssetPath));
	}

	// 5. Check new row name doesn't already exist
	const FName NewFName(*NewRowName);
	if (Table->FindRowUnchecked(NewFName) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("Row '%s' already exists in data table '%s'. Choose a different name."), *NewRowName, *AssetPath));
	}

	// 6. Duplicate the row
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] DataTable Duplicate Row")));
	uint8* NewRow = FDataTableEditorUtils::DuplicateRow(Table, SourceFName, NewFName);
	if (!NewRow)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to duplicate row '%s' to '%s' in data table '%s'."), *SourceRow, *NewRowName, *AssetPath));
	}

	// 7. Auto-save
	FString SaveError;
	bool bSaved = ClaireonDataTableHelpers::SaveDataTable(Table, SaveError);

	FString Output = FString::Printf(TEXT("Row '%s' duplicated to '%s' in '%s'."), *SourceRow, *NewRowName, *AssetPath);
	if (!bSaved)
	{
		Output += FString::Printf(TEXT("\nWarning: %s"), *SaveError);
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.datatable.duplicate_row: %s"), *Output);
	return MakeSuccessResult(nullptr, Output);
}
