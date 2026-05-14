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

	// ---------------------------------------------------------------------
	// Context-data helpers (shared by chooser and proxyasset context edits).
	//
	// Caller owns recompile. Chooser callers must call
	// Chooser->Compile(true) when bContextChanged is true. ProxyAsset
	// callers have no such obligation (UProxyAsset has no Compile).
	// ---------------------------------------------------------------------

	/** Appends a new FContextObjectTypeBase-derived entry to ContextData.
	 *  TypeString: "struct" or "class" (case-insensitive).
	 *  NameString: struct or class name (passed to FindObject / LoadObject).
	 *  DirectionString: passed through ParseDirection.
	 *  Sets bContextChanged = true iff ContextData was mutated.
	 *  Returns true on success. On failure OutError describes the cause. */
	bool AddContextParameter(
		TArray<FInstancedStruct>& ContextData,
		const FString& TypeString,
		const FString& NameString,
		const FString& DirectionString,
		bool& bContextChanged,
		FString& OutError);

	/** Removes ContextData[Index]. Returns false with OutError if Index is out of bounds.
	 *  Sets bContextChanged on mutation. */
	bool RemoveContextParameter(
		TArray<FInstancedStruct>& ContextData,
		int32 Index,
		bool& bContextChanged,
		FString& OutError);

	/** Sets Direction on ContextData[Index]'s FContextObjectTypeBase.
	 *  Returns false with OutError if Index is out of bounds or entry is not a FContextObjectTypeBase.
	 *  Sets bContextChanged on mutation. */
	bool SetContextParameterDirection(
		TArray<FInstancedStruct>& ContextData,
		int32 Index,
		const FString& DirectionString,
		bool& bContextChanged,
		FString& OutError);

} // namespace ClaireonChooserHelpers
