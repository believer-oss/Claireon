// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableCompositeRefresh.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonScopedAssetLock.h"
#include "Engine/DataTable.h"
#include "Engine/CompositeDataTable.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_DataTableCompositeRefresh::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableCompositeRefresh::GetOperation() const { return TEXT("composite_refresh"); }

FString ClaireonTool_DataTableCompositeRefresh::GetDescription() const
{
	return TEXT("Refresh the cached row map of a composite data table and save it. Immediate and non-session: the rebuild and the save both happen in this call under a scoped asset lock, so there is no open/close pair to manage. Use this after editing parent tables outside the normal MCP flow, or as the trailing call in a batch-edit sequence (refresh_composites=false).");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableCompositeRefresh::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> AssetPathProp = MakeShared<FJsonObject>();
	AssetPathProp->SetStringField(TEXT("type"), TEXT("string"));
	AssetPathProp->SetStringField(TEXT("description"), TEXT("Asset path to the composite data table (e.g. /Game/Data/DT_Composite_Items)"));
	Properties->SetObjectField(TEXT("asset_path"), AssetPathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableCompositeRefresh::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString LoadError;
	UDataTable* DataTable = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, LoadError);
	if (!IsValid(DataTable))
	{
		return MakeErrorResult(LoadError);
	}

	UCompositeDataTable* Composite = ClaireonDataTableHelpers::AsCompositeDataTable(DataTable);
	if (!IsValid(Composite))
	{
		return MakeErrorResult(TEXT("Asset is not a Composite Data Table; nothing to refresh."));
	}

	// Intentionally skip EnsureWritable -- it rejects all composites by design (I1); this tool's job is to operate on one.
	FString RefreshError;
	if (!ClaireonDataTableHelpers::RefreshCompositeDataTable(Composite, RefreshError))
	{
		return MakeErrorResult(RefreshError);
	}

	const TArray<FString> ParentPaths = ClaireonDataTableHelpers::GetCompositeParentTablePaths(Composite);
	const int32 RowCount = Composite->GetRowMap().Num();
	const FString TableName = FPaths::GetBaseFilename(AssetPath);

	TArray<TSharedPtr<FJsonValue>> ParentsArray;
	for (const FString& ParentPath : ParentPaths)
	{
		ParentsArray.Add(MakeShared<FJsonValueString>(ParentPath));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("asset_path"), AssetPath);
	Data->SetArrayField(TEXT("parents"), ParentsArray);
	Data->SetNumberField(TEXT("row_count"), RowCount);
	Data->SetBoolField(TEXT("refreshed"), true);

	const FString Summary = FString::Printf(TEXT("Refreshed composite '%s' (%d rows)"), *TableName, RowCount);

	return MakeSuccessResult(Data, Summary);
}
