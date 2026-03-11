// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FProperty;
class UDataTable;
class UScriptStruct;

/**
 * Shared utility functions for Data Table MCP tools.
 * Provides asset loading, property reflection, and save utilities.
 */
namespace ClaireonDataTableHelpers
{
	struct FColumnDef
	{
		FString Name;
		FString CppType;
		const FProperty* Property = nullptr;
	};

	UDataTable* LoadDataTableAsset(const FString& AssetPath, FString& OutError);
	bool IsCompositeDataTable(const UDataTable* DataTable);
	bool EnsureWritable(const UDataTable* DataTable, FString& OutError);
	bool ValidateRowName(const FString& Name, FString& OutError);
	bool SaveDataTable(UDataTable* DataTable, FString& OutError);

	TArray<FColumnDef> GetColumnDefinitions(const UScriptStruct* RowStruct);

	FString SerializeRowToText(const UDataTable* DataTable, FName RowName, const TArray<FString>* Columns = nullptr);
	FString GetPropertyValueAsString(const uint8* RowData, const FProperty* Property);

	bool SetPropertyValueFromString(uint8* RowData, const FProperty* Property, const FString& Value, FString& OutError);
	bool SetPropertyValues(UDataTable* DataTable, FName RowName, const TSharedPtr<FJsonObject>& Values, FString& OutError);
} // namespace ClaireonDataTableHelpers
