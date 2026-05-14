// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableImportCsv.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonScopedAssetLock.h"
#include "Engine/DataTable.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"

FString ClaireonTool_DataTableImportCsv::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableImportCsv::GetOperation() const { return TEXT("import_csv"); }

FString ClaireonTool_DataTableImportCsv::GetDescription() const
{
	return TEXT("Import CSV data into a data table");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableImportCsv::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Fully qualified asset path (e.g., /Game/Data/DT_MyTable)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	TSharedPtr<FJsonObject> CsvProp = MakeShared<FJsonObject>();
	CsvProp->SetStringField(TEXT("type"), TEXT("string"));
	CsvProp->SetStringField(TEXT("description"), TEXT("CSV string to import (same format as export_csv output)"));
	Properties->SetObjectField(TEXT("csv"), CsvProp);

	TSharedPtr<FJsonObject> PreserveExistingProp = MakeShared<FJsonObject>();
	PreserveExistingProp->SetStringField(TEXT("type"), TEXT("boolean"));
	PreserveExistingProp->SetStringField(TEXT("description"), TEXT("If true, keeps existing rows not present in the CSV (default: false)"));
	Properties->SetObjectField(TEXT("preserve_existing"), PreserveExistingProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("csv")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableImportCsv::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString CsvInput;
	if (!Arguments->TryGetStringField(TEXT("csv"), CsvInput) || CsvInput.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: csv"));
	}

	bool bPreserveExisting = false;
	Arguments->TryGetBoolField(TEXT("preserve_existing"), bPreserveExisting);

	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.datatable.import_csv: asset_path=%s, preserve_existing=%s"),
		*AssetPath, bPreserveExisting ? TEXT("true") : TEXT("false"));

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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Import DataTable CSV")));
	Table->Modify();

	if (bPreserveExisting)
	{
		Table->bPreserveExistingValues = true;
	}

	TArray<FString> Problems = Table->CreateTableFromCSVString(CsvInput);

	if (bPreserveExisting)
	{
		Table->bPreserveExistingValues = false;
	}

	int32 RowCount = Table->GetRowMap().Num();

	FString SaveError;
	bool bSaved = ClaireonDataTableHelpers::SaveDataTable(Table, SaveError);

	FString Output = FString::Printf(TEXT("Imported %d rows."), RowCount);

	if (Problems.Num() > 0)
	{
		Output += TEXT("\nProblems:");
		for (const FString& Problem : Problems)
		{
			Output += FString::Printf(TEXT("\n- %s"), *Problem);
		}
	}

	if (bSaved)
	{
		Output += TEXT("\nAsset saved successfully.");
	}
	else
	{
		Output += FString::Printf(TEXT("\nWarning: %s"), *SaveError);
	}

	return MakeSuccessResult(nullptr, Output);
}
