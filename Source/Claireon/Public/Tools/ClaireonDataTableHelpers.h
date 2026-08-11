// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FProperty;
class UCompositeDataTable;
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

	UCompositeDataTable* AsCompositeDataTable(UDataTable* DataTable);
	bool RefreshCompositeDataTable(UCompositeDataTable* Composite, FString& OutError);          // Stage 002
	TArray<FString> RefreshDependentComposites(const UDataTable* ModifiedTable, FString& OutError); // Stage 002

	// Shared auto-refresh: refresh dependent composites, write refreshed_composites/refresh_warning into Data, return a summary suffix.
	FString RefreshDependentCompositesResult(UDataTable* Table, bool bRefreshComposites, const TSharedPtr<class FJsonObject>& Data);

	// Live parent UDataTable* objects (ParentTables, reflected) for a composite, in override order.
	TArray<UDataTable*> GetCompositeParentTableObjects(const UCompositeDataTable* Composite);

	// Parent asset paths (ParentTables, reflected) for a composite, in override order. Stage 003.
	TArray<FString> GetCompositeParentTablePaths(const UCompositeDataTable* Composite);
} // namespace ClaireonDataTableHelpers
