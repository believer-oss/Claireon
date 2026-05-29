// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableGetRowStructured.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonStructReflection.h"
#include "Engine/DataTable.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

FString ClaireonTool_DataTableGetRowStructured::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableGetRowStructured::GetOperation() const { return TEXT("get_row"); }

FString ClaireonTool_DataTableGetRowStructured::GetDescription() const
{
	return TEXT(
		"Get a single DataTable row as a nested JSON tree mirroring the row struct's property layout. "
		"BP user-defined struct GUID suffixes are stripped (PossibleLines_17_<GUID> -> PossibleLines). "
		"FText is exploded into { text, namespace, key }. "
		"TMap is emitted as an array of { key, value } pairs so non-string keys (FGameplayTag, structs, etc.) round-trip without lossy stringification. "
		"TArray and TSet are emitted as JSON arrays. "
		"Soft and hard object references emit the asset path string (soft references never force-load). "
		"Enums emit { value, name } objects. "
		"Optional 'columns' filter accepts friendly or raw property names, case-insensitive. "
		"Optional 'include_schema' flag adds a sibling top-level 'schema' field for callers that need the struct shape alongside values. "
		"For bulk row pulls use datatable_export_json or datatable_get_rows with a columns filter."
	);
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableGetRowStructured::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// asset_path -- required
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Fully qualified asset path (e.g. /Game/Data/DT_Items)"));
		Properties->SetObjectField(TEXT("asset_path"), Prop);
	}

	// row_name -- required
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Name of the row to retrieve"));
		Properties->SetObjectField(TEXT("row_name"), Prop);
	}

	// columns -- optional array of strings
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("array"));
		Prop->SetStringField(TEXT("description"), TEXT("Specific column names to return (friendly or raw property name; case-insensitive). Omit for all columns."));
		{
			TSharedPtr<FJsonObject> ItemsObj = MakeShared<FJsonObject>();
			ItemsObj->SetStringField(TEXT("type"), TEXT("string"));
			Prop->SetObjectField(TEXT("items"), ItemsObj);
		}
		Properties->SetObjectField(TEXT("columns"), Prop);
	}

	// include_schema -- optional bool
	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("boolean"));
		Prop->SetStringField(TEXT("description"), TEXT("When true, the response includes a sibling top-level 'schema' field describing each column's struct shape. Defaults to false."));
		Properties->SetObjectField(TEXT("include_schema"), Prop);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("row_name")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableGetRowStructured::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

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

	// Optional column filter
	TArray<FString> RequestedColumns;
	if (Arguments->HasField(TEXT("columns")))
	{
		const TArray<TSharedPtr<FJsonValue>>* ColsArrayPtr = nullptr;
		if (Arguments->TryGetArrayField(TEXT("columns"), ColsArrayPtr) && ColsArrayPtr)
		{
			for (const TSharedPtr<FJsonValue>& Val : *ColsArrayPtr)
			{
				FString ColName;
				if (Val.IsValid() && Val->TryGetString(ColName) && !ColName.IsEmpty())
				{
					RequestedColumns.Add(ColName);
				}
			}
		}
	}

	// Optional include_schema flag
	bool bIncludeSchema = false;
	Arguments->TryGetBoolField(TEXT("include_schema"), bIncludeSchema);

	// Build the per-field values object.
	TArray<FString> Warnings;
	TSharedPtr<FJsonObject> ValuesObj = MakeShared<FJsonObject>();

	// We iterate the row struct ourselves (rather than delegating to SerializeStructInstance)
	// so the columns filter applies at the emission step while still letting collision suffixing
	// see all properties consistently.
	TMap<FString, int32> EmittedKeyCounters;
	int32 FieldCount = 0;
	TSharedPtr<FJsonObject> SchemaObj;
	if (bIncludeSchema)
	{
		SchemaObj = MakeShared<FJsonObject>();
	}

	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		const FProperty* Prop = *It;
		if (!Prop)
		{
			continue;
		}

		const FString BaseKey = ClaireonStructReflection::GetFriendlyPropertyName(Prop);
		const FString RawName = Prop->GetName();

		// Friendly-name collision tracking happens for ALL properties so suffix ordering is stable.
		FString EmitKey = BaseKey;
		int32* ExistingCounter = EmittedKeyCounters.Find(BaseKey);
		if (ExistingCounter)
		{
			*ExistingCounter += 1;
			EmitKey = FString::Printf(TEXT("%s#%d"), *BaseKey, *ExistingCounter);
		}
		else
		{
			EmittedKeyCounters.Add(BaseKey, 1);
		}

		// Apply columns filter against friendly OR raw name (and the suffixed emit key for tie-breaking).
		if (RequestedColumns.Num() > 0)
		{
			bool bWanted = false;
			for (const FString& Col : RequestedColumns)
			{
				if (Col.Equals(BaseKey, ESearchCase::IgnoreCase) ||
					Col.Equals(RawName, ESearchCase::IgnoreCase) ||
					Col.Equals(EmitKey, ESearchCase::IgnoreCase))
				{
					bWanted = true;
					break;
				}
			}
			if (!bWanted)
			{
				continue;
			}
		}

		const void* FieldPtr = Prop->ContainerPtrToValuePtr<void>(RowData);
		TSharedPtr<FJsonValue> FieldVal = ClaireonStructReflection::SerializePropertyValue(Prop, FieldPtr);
		ValuesObj->SetField(EmitKey, FieldVal);
		++FieldCount;

		// Schema half mirrors the values tree by emit key.
		// For struct-typed columns, emit the per-struct schema via SerializeStructSchema so callers
		// get the field shape. For non-struct columns, fall back to per-field SerializeProperty since
		// SerializeStructSchema is undefined on non-struct property types.
		if (bIncludeSchema && SchemaObj.IsValid())
		{
			if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
			{
				TSharedPtr<FJsonObject> ColSchema = ClaireonStructReflection::SerializeStructSchema(StructProp->Struct, /*bIncludeDefaults=*/false, /*bIncludeMetadata=*/false);
				SchemaObj->SetObjectField(EmitKey, ColSchema);
			}
			else
			{
				TSharedPtr<FJsonObject> FieldSchema = ClaireonStructReflection::SerializeProperty(RowStruct, Prop, /*bIncludeDefaults=*/false, /*bIncludeMetadata=*/false);
				SchemaObj->SetObjectField(EmitKey, FieldSchema);
			}
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("table_path"), AssetPath);
	Data->SetStringField(TEXT("row_name"), RowNameStr);
	Data->SetObjectField(TEXT("values"), ValuesObj);
	if (bIncludeSchema && SchemaObj.IsValid())
	{
		Data->SetObjectField(TEXT("schema"), SchemaObj);
	}

	const FString TableName = FPaths::GetBaseFilename(AssetPath);
	const FString Summary = FString::Printf(TEXT("Row '%s' from %s: %d fields"), *RowNameStr, *TableName, FieldCount);

	FToolResult Result = MakeSuccessResult(Data, Summary);
	Result.Warnings = MoveTemp(Warnings);
	return Result;
}
