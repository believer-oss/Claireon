// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UChooserTable;
struct FInstancedStruct;
struct FChooserColumnBase;

/**
 * Shared utility functions for Chooser Table MCP tools.
 * Provides asset loading, column/row introspection, and save utilities.
 */
namespace ClaireonChooserHelpers
{
	/** Load and validate a UChooserTable from an asset path. */
	UChooserTable* LoadChooserTableAsset(const FString& AssetPath, FString& OutError);

	/** Save a modified chooser table to disk. */
	bool SaveChooserTable(UChooserTable* Chooser, FString& OutError);

	/** Serialize context data parameters to a JSON array. */
	TArray<TSharedPtr<FJsonValue>> SerializeContextData(const TArray<FInstancedStruct>& ContextData);

	/** Serialize a single column (FInstancedStruct wrapping FChooserColumnBase) to JSON.
	 *  If ContextData is provided, resolves property bindings to show actual types. */
	TSharedPtr<FJsonObject> SerializeColumn(const FInstancedStruct& ColumnStruct, int32 ColumnIndex, const TArray<FInstancedStruct>* ContextData = nullptr);

	/** Serialize column cell value for a specific row (dispatches by column type). */
	TSharedPtr<FJsonValue> SerializeColumnCellValue(const FInstancedStruct& ColumnStruct, int32 RowIndex);

	/** Serialize a single row result (FInstancedStruct wrapping FObjectChooserBase) to JSON. */
	TSharedPtr<FJsonObject> SerializeRowResult(const FInstancedStruct& ResultStruct);

	/** Serialize an FInstancedStruct's fields to JSON using reflection. */
	TSharedPtr<FJsonObject> SerializeInstancedStructToJson(const FInstancedStruct& Struct);

	/** Extract the property binding info from a column's InputValue FInstancedStruct.
	 *  If ContextData is provided, resolves the binding chain to show actual property types. */
	TSharedPtr<FJsonObject> SerializePropertyBinding(const FInstancedStruct& ColumnStruct, const TArray<FInstancedStruct>* ContextData = nullptr);

	/** Convert EObjectChooserResultType to string. */
	FString ResultTypeToString(uint8 ResultType);

	/** Set a column cell value from JSON input. Dispatches by column type. */
	bool SetColumnCellValue(FInstancedStruct& ColumnStruct, int32 RowIndex,
		const TSharedPtr<FJsonValue>& Value, FString& OutError);

	/** Construct a row result FInstancedStruct from a type name and asset path. */
	bool MakeRowResult(const FString& ResultType, const FString& ResultValue,
		FInstancedStruct& OutResult, FString& OutError);

	/** Validate a new asset path, returning canonical path and asset name. */
	bool ValidateNewAssetPath(const FString& InPath, FString& OutCanonPath, FString& OutAssetName, FString& OutError);

	/** Save a newly created asset to disk. */
	bool SaveNewAsset(UObject* Asset, FString& OutError);

	/** Parse a direction string to EContextObjectDirection. */
	uint8 ParseDirection(const FString& Str);

} // namespace ClaireonChooserHelpers
