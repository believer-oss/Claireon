// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableImportJson.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonScopedAssetLock.h"
#include "Engine/DataTable.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"

FString ClaireonTool_DataTableImportJson::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableImportJson::GetOperation() const { return TEXT("import_json"); }

FString ClaireonTool_DataTableImportJson::GetDescription() const
{
    return TEXT("Replace entire table contents from a JSON payload. Stateless / non-session: writes the asset directly by path without opening any editing session.");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableImportJson::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Fully qualified asset path (e.g., /Game/Data/DT_MyTable)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	TSharedPtr<FJsonObject> JsonProp = MakeShared<FJsonObject>();
	JsonProp->SetStringField(TEXT("type"), TEXT("string"));
	JsonProp->SetStringField(TEXT("description"), TEXT("JSON string to import (same format as export_json output)"));
	Properties->SetObjectField(TEXT("json"), JsonProp);

	// refresh_composites - optional
	TSharedPtr<FJsonObject> RefreshCompositesProp = MakeShared<FJsonObject>();
	RefreshCompositesProp->SetStringField(TEXT("type"), TEXT("boolean"));
	RefreshCompositesProp->SetStringField(TEXT("description"), TEXT("After saving, refresh any composite data tables that aggregate this table (default: true). Set false for batch edits; follow with an explicit datatable_composite_refresh."));
	Properties->SetObjectField(TEXT("refresh_composites"), RefreshCompositesProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("json")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableImportJson::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString JsonInput;
	if (!Arguments->TryGetStringField(TEXT("json"), JsonInput) || JsonInput.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: json"));
	}

	bool bRefreshComposites = true;
	if (Arguments->HasField(TEXT("refresh_composites")))
	{
		bRefreshComposites = Arguments->GetBoolField(TEXT("refresh_composites"));
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.datatable.import_json: asset_path=%s"), *AssetPath);

	FString Error;
	UDataTable* Table = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, Error);
	if (!IsValid(Table))
	{
		return MakeErrorResult(Error);
	}

	if (!ClaireonDataTableHelpers::EnsureWritable(Table, Error))
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Import DataTable JSON")));
	Table->Modify();
	TArray<FString> Problems = Table->CreateTableFromJSONString(JsonInput);

	int32 RowCount = Table->GetRowMap().Num();

	FString SaveError;
	bool bSaved = ClaireonDataTableHelpers::SaveDataTable(Table, SaveError);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
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
		// One refresh for the whole import, not per row.
		Output += ClaireonDataTableHelpers::RefreshDependentCompositesResult(Table, bRefreshComposites, Data);
	}
	else
	{
		Output += FString::Printf(TEXT("\nWarning: %s"), *SaveError);
	}

	return MakeSuccessResult(Data, Output);
}
