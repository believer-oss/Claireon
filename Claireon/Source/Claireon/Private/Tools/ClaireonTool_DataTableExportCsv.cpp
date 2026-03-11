// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableExportCsv.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "Engine/DataTable.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_DataTableExportCsv::GetName() const
{
	return TEXT("editor.datatable.export_csv");
}

FString ClaireonTool_DataTableExportCsv::GetDescription() const
{
	return TEXT("Export table as CSV");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableExportCsv::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Fully qualified asset path (e.g., /Game/Data/DT_MyTable)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableExportCsv::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.datatable.export_csv: asset_path=%s"), *AssetPath);

	FString Error;
	UDataTable* Table = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, Error);
	if (!Table)
	{
		return MakeErrorResult(Error);
	}

	FString CsvOutput = Table->GetTableAsCSV(EDataTableExportFlags::None);
	if (CsvOutput.StartsWith(TEXT("Missing RowStruct")))
	{
		return MakeErrorResult(FString::Printf(TEXT("Table has no row struct: %s"), *AssetPath));
	}

	return MakeSuccessResult(nullptr, CsvOutput);
}
