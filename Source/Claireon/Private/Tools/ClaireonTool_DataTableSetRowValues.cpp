// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableSetRowValues.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonScopedAssetLock.h"
#include "Engine/DataTable.h"
#include "DataTableEditorUtils.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"

FString ClaireonTool_DataTableSetRowValues::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableSetRowValues::GetOperation() const { return TEXT("set_row_values"); }

FString ClaireonTool_DataTableSetRowValues::GetDescription() const
{
    return TEXT("Set one or more property values on an existing data table row. Stateless / non-session: writes the asset directly by path without opening any session.");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableSetRowValues::GetInputSchema() const
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
	RowNameProp->SetStringField(TEXT("description"), TEXT("Name of the row to modify"));
	Properties->SetObjectField(TEXT("row_name"), RowNameProp);

	// values - required
	TSharedPtr<FJsonObject> ValuesProp = MakeShared<FJsonObject>();
	ValuesProp->SetStringField(TEXT("type"), TEXT("object"));
	ValuesProp->SetStringField(TEXT("description"), TEXT("Property name to value pairs (ImportText format)"));
	Properties->SetObjectField(TEXT("values"), ValuesProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("row_name")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("values")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableSetRowValues::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	const TSharedPtr<FJsonObject>* ValuesPtr = nullptr;
	if (!Arguments->TryGetObjectField(TEXT("values"), ValuesPtr) || !ValuesPtr || !(*ValuesPtr).IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: values"));
	}
	const TSharedPtr<FJsonObject> Values = *ValuesPtr;

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
	const uint8* RowData = DataTable->FindRowUnchecked(RowName);
	if (!RowData)
	{
		return MakeErrorResult(FString::Printf(TEXT("Row '%s' not found in %s"), *RowNameStr, *AssetPath));
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();

	// Capture old values before modifying
	TSharedPtr<FJsonObject> OldValues = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> NewValues = MakeShared<FJsonObject>();
	TArray<FString> UpdatedFields;

	for (const auto& Pair : Values->Values)
	{
		const FString Key(*Pair.Key);

		const FProperty* Prop = RowStruct ? RowStruct->FindPropertyByName(FName(*Key)) : nullptr;
		if (!Prop)
		{
			return MakeErrorResult(FString::Printf(TEXT("Property '%s' not found in row struct"), *Key));
		}

		// Snapshot old value
		OldValues->SetStringField(Key, ClaireonDataTableHelpers::GetPropertyValueAsString(RowData, Prop));
		UpdatedFields.Add(Key);
	}

	// Apply the values
	FString SetError;
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] DataTable Set Row Values")));
	DataTable->Modify();
	if (!ClaireonDataTableHelpers::SetPropertyValues(DataTable, RowName, Values, SetError))
	{
		return MakeErrorResult(SetError);
	}

	// Capture new values after modification
	const uint8* UpdatedRowData = DataTable->FindRowUnchecked(RowName);
	for (const FString& Key : UpdatedFields)
	{
		const FProperty* Prop = RowStruct->FindPropertyByName(FName(*Key));
		if (Prop && UpdatedRowData)
		{
			NewValues->SetStringField(Key, ClaireonDataTableHelpers::GetPropertyValueAsString(UpdatedRowData, Prop));
		}
	}

	// Save
	FString SaveError;
	if (!ClaireonDataTableHelpers::SaveDataTable(DataTable, SaveError))
	{
		return MakeErrorResult(SaveError);
	}

	const FString TableName = FPaths::GetBaseFilename(AssetPath);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), AssetPath);
	Data->SetStringField(TEXT("row_name"), RowNameStr);
	Data->SetNumberField(TEXT("updated_fields"), UpdatedFields.Num());
	Data->SetObjectField(TEXT("old_values"), OldValues);
	Data->SetObjectField(TEXT("new_values"), NewValues);

	const FString Summary = FString::Printf(TEXT("Updated %d fields on row '%s'"),
		UpdatedFields.Num(), *RowNameStr);

	return MakeSuccessResult(Data, Summary);
}
