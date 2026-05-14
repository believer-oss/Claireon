// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonChooserTool_SetColumnValue.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Chooser.h"
#include "ScopedTransaction.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "StructUtils/InstancedStruct.h"

FString ClaireonTool_ChooserSetColumnValue::GetCategory() const { return TEXT("chooser"); }
FString ClaireonTool_ChooserSetColumnValue::GetOperation() const { return TEXT("set_column_value"); }

FString ClaireonTool_ChooserSetColumnValue::GetDescription() const
{
	return TEXT("Set a column cell value for a specific row in a ChooserTable. "
		"The value format depends on the column type: "
		"GameplayTag: comma-separated tags or array; "
		"Bool: 'true'/'false'/'any'; "
		"Enum: {\"value\": \"Name\", \"comparison\": \"MatchEqual\"} or just the value name; "
		"FloatRange: {\"min\": N, \"max\": N, \"no_min\": bool, \"no_max\": bool}; "
		"Object: asset path string (defaults Comparison=MatchEqual) or {\"value\": \"/Game/...\", \"comparison\": \"MatchEqual|MatchNotEqual|MatchAny\"}; "
		"OutputStruct: {field_name: value, ...}; "
		"OutputObject: asset path string.");
}

TSharedPtr<FJsonObject> ClaireonTool_ChooserSetColumnValue::GetInputSchema() const
{
	FToolSchemaBuilder S;
	S.AddString(TEXT("asset_path"), TEXT("Path to the ChooserTable asset"), true);
	S.AddInteger(TEXT("row_index"), TEXT("Row index"), true);
	S.AddInteger(TEXT("column_index"), TEXT("Column index"), true);
	S.AddString(TEXT("value"), TEXT("Value to set (format depends on column type; use JSON string for structured values)"), true);
	return S.Build();
}

IClaireonTool::FToolResult ClaireonTool_ChooserSetColumnValue::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	double RowIdxDouble, ColIdxDouble;
	if (!Arguments->TryGetNumberField(TEXT("row_index"), RowIdxDouble))
	{
		return MakeErrorResult(TEXT("Missing required parameter: row_index"));
	}
	if (!Arguments->TryGetNumberField(TEXT("column_index"), ColIdxDouble))
	{
		return MakeErrorResult(TEXT("Missing required parameter: column_index"));
	}

	int32 RowIndex = static_cast<int32>(RowIdxDouble);
	int32 ColIndex = static_cast<int32>(ColIdxDouble);

	FString Error;
	UChooserTable* Chooser = ClaireonChooserHelpers::LoadChooserTableAsset(AssetPath, Error);
	if (!Chooser)
	{
		return MakeErrorResult(Error);
	}

	if (!Chooser->ColumnsStructs.IsValidIndex(ColIndex))
	{
		return MakeErrorResult(FString::Printf(TEXT("Column index %d out of bounds (column count: %d)"),
			ColIndex, Chooser->ColumnsStructs.Num()));
	}

	// Get the value field - could be string, object, array, etc.
	TSharedPtr<FJsonValue> ValueField = Arguments->TryGetField(TEXT("value"));
	if (!ValueField.IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: value"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set ChooserTable Column Value")));
	Chooser->Modify();

	if (!ClaireonChooserHelpers::SetColumnCellValue(Chooser->ColumnsStructs[ColIndex], RowIndex, ValueField, Error))
	{
		return MakeErrorResult(Error);
	}

	if (!ClaireonChooserHelpers::SaveChooserTable(Chooser, Error))
	{
		return MakeErrorResult(Error);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), Chooser->GetPathName());
	Data->SetNumberField(TEXT("row_index"), RowIndex);
	Data->SetNumberField(TEXT("column_index"), ColIndex);

	return MakeSuccessResult(Data, FString::Printf(TEXT("Set column %d value at row %d"), ColIndex, RowIndex));
}
