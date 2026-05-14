// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_DataTableMoveRow.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonLog.h"
#include "ClaireonScopedAssetLock.h"
#include "Engine/DataTable.h"
#include "DataTableEditorUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"

FString ClaireonTool_DataTableMoveRow::GetCategory() const { return TEXT("datatable"); }
FString ClaireonTool_DataTableMoveRow::GetOperation() const { return TEXT("move_row"); }

FString ClaireonTool_DataTableMoveRow::GetDescription() const
{
	return TEXT("Reorder a row up or down");
}

TSharedPtr<FJsonObject> ClaireonTool_DataTableMoveRow::GetInputSchema() const
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
	RowNameProp->SetStringField(TEXT("description"), TEXT("Name of the row to move"));
	Properties->SetObjectField(TEXT("row_name"), RowNameProp);

	// direction - required
	TSharedPtr<FJsonObject> DirectionProp = MakeShared<FJsonObject>();
	DirectionProp->SetStringField(TEXT("type"), TEXT("string"));
	DirectionProp->SetStringField(TEXT("description"), TEXT("Direction to move the row: \"up\" or \"down\""));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("up")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("down")));
		DirectionProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("direction"), DirectionProp);

	// count - optional
	TSharedPtr<FJsonObject> CountProp = MakeShared<FJsonObject>();
	CountProp->SetStringField(TEXT("type"), TEXT("integer"));
	CountProp->SetStringField(TEXT("description"), TEXT("Number of positions to move the row (default: 1)"));
	Properties->SetObjectField(TEXT("count"), CountProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("asset_path")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("row_name")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("direction")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_DataTableMoveRow::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString Direction;
	if (!Arguments->TryGetStringField(TEXT("direction"), Direction) || Direction.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: direction"));
	}

	if (Direction != TEXT("up") && Direction != TEXT("down"))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid direction '%s'. Must be \"up\" or \"down\"."), *Direction));
	}

	int32 Count = 1;
	if (Arguments->HasField(TEXT("count")))
	{
		Count = FMath::Max(1, static_cast<int32>(Arguments->GetNumberField(TEXT("count"))));
	}

	// 2. Load and validate table
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

	// 3. Check row exists
	const FName RowFName(*RowName);
	if (Table->FindRowUnchecked(RowFName) == nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("Row '%s' not found in data table '%s'."), *RowName, *AssetPath));
	}

	// 4. Map direction string to enum
	FDataTableEditorUtils::ERowMoveDirection MoveDirection = (Direction == TEXT("up"))
		? FDataTableEditorUtils::ERowMoveDirection::Up
		: FDataTableEditorUtils::ERowMoveDirection::Down;

	// 5. Move the row
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] DataTable Move Row")));
	bool bMoved = FDataTableEditorUtils::MoveRow(Table, RowFName, MoveDirection, Count);
	if (!bMoved)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to move row '%s' %s in data table '%s'. Row may already be at the %s."),
			*RowName, *Direction, *AssetPath, Direction == TEXT("up") ? TEXT("top") : TEXT("bottom")));
	}

	// 6. Find new position after move
	int32 NewIndex = INDEX_NONE;
	TArray<FName> RowNames = Table->GetRowNames();
	for (int32 i = 0; i < RowNames.Num(); ++i)
	{
		if (RowNames[i] == RowFName)
		{
			NewIndex = i;
			break;
		}
	}

	// 7. Auto-save
	FString SaveError;
	bool bSaved = ClaireonDataTableHelpers::SaveDataTable(Table, SaveError);

	FString Output;
	if (NewIndex != INDEX_NONE)
	{
		Output = FString::Printf(TEXT("Row '%s' moved %s by %d position(s). New index: %d (of %d) in '%s'."),
			*RowName, *Direction, Count, NewIndex, RowNames.Num() - 1, *AssetPath);
	}
	else
	{
		Output = FString::Printf(TEXT("Row '%s' moved %s by %d position(s) in '%s'."),
			*RowName, *Direction, Count, *AssetPath);
	}

	if (!bSaved)
	{
		Output += FString::Printf(TEXT("\nWarning: %s"), *SaveError);
	}

	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.datatable.move_row: %s"), *Output);
	return MakeSuccessResult(nullptr, Output);
}
