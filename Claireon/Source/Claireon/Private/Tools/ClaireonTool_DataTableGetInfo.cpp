// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableGetInfo.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "Engine/DataTable.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_DataTableGetInfo::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableGetInfo::GetOperation() const { return TEXT("get_info"); }

FString ClaireonTool_DataTableGetInfo::GetDescription() const
{
	return TEXT("Get structural metadata about a data table Ã¢Â€Â” row struct, column definitions, row count");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableGetInfo::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path - required
	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Fully qualified asset path (e.g. /Game/Data/DT_Items)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableGetInfo::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	const FString StructName = RowStruct ? RowStruct->GetName() : TEXT("(unknown)");

	TArray<ClaireonDataTableHelpers::FColumnDef> Columns = ClaireonDataTableHelpers::GetColumnDefinitions(RowStruct);

	// Build columns array
	TArray<TSharedPtr<FJsonValue>> ColumnsArray;
	for (const ClaireonDataTableHelpers::FColumnDef& Col : Columns)
	{
		TSharedPtr<FJsonObject> ColObj = MakeShared<FJsonObject>();
		ColObj->SetStringField(TEXT("name"), Col.Name);
		ColObj->SetStringField(TEXT("type"), Col.CppType);
		ColumnsArray.Add(MakeShared<FJsonValueObject>(ColObj));
	}

	const int32 RowCount = DataTable->GetRowMap().Num();
	const FString TableName = FPaths::GetBaseFilename(AssetPath);

	const bool bIsComposite = ClaireonDataTableHelpers::IsCompositeDataTable(DataTable);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), AssetPath);
	Data->SetStringField(TEXT("row_struct"), StructName);
	Data->SetNumberField(TEXT("row_count"), RowCount);
	Data->SetArrayField(TEXT("columns"), ColumnsArray);
	Data->SetBoolField(TEXT("is_composite"), bIsComposite);

	const FString CompositeStr = bIsComposite ? TEXT(" [composite]") : TEXT("");
	const FString Summary = FString::Printf(TEXT("%s: %d rows, struct %s (%d columns)%s"),
		*TableName, RowCount, *StructName, Columns.Num(), *CompositeStr);

	return MakeSuccessResult(Data, Summary);
}
