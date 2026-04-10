// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonLog.h"
#include "ClaireonSafeExec.h"
#include "Engine/DataTable.h"
#include "Engine/CompositeDataTable.h"
#include "FileHelpers.h"
#include "UObject/UnrealType.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace ClaireonDataTableHelpers
{

UDataTable* LoadDataTableAsset(const FString& AssetPath, FString& OutError)
{
	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		OutError = ResolveResult.Error;
		return nullptr;
	}
	const FString ResolvedPath = ResolveResult.ResolvedPath.Path;

	UObject* LoadedObj = FSoftObjectPath(ResolvedPath).TryLoad();
	if (!LoadedObj)
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
		return nullptr;
	}

	UDataTable* DataTable = Cast<UDataTable>(LoadedObj);
	if (!DataTable)
	{
		OutError = FString::Printf(TEXT("Asset at %s is not a DataTable (actual type: %s)"), *ResolvedPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	return DataTable;
}

bool IsCompositeDataTable(const UDataTable* DataTable)
{
	return DataTable && Cast<UCompositeDataTable>(DataTable) != nullptr;
}

bool EnsureWritable(const UDataTable* DataTable, FString& OutError)
{
	if (!DataTable)
	{
		OutError = TEXT("Data table is null");
		return false;
	}

	if (IsCompositeDataTable(DataTable))
	{
		OutError = TEXT("Cannot modify rows on a Composite Data Table. Modify the source table instead.");
		return false;
	}

	return true;
}

bool ValidateRowName(const FString& Name, FString& OutError)
{
	if (Name.IsEmpty())
	{
		OutError = TEXT("Row name is empty");
		return false;
	}

	FName TestName(*Name);
	if (!TestName.IsValid() || TestName.IsNone())
	{
		OutError = FString::Printf(TEXT("Invalid row name '%s'. Row names must be valid FName values (letters, digits, underscores)."), *Name);
		return false;
	}

	return true;
}

bool SaveDataTable(UDataTable* DataTable, FString& OutError)
{
	if (!DataTable)
	{
		OutError = TEXT("Data table is null");
		return false;
	}

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(DataTable->GetPackage());
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		OutError = TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor.");
		return false;
	}
	bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
	if (!bSaved)
	{
		OutError = FString::Printf(TEXT("Failed to save package: %s"), *DataTable->GetPackage()->GetName());
	}
	return bSaved;
}

TArray<FColumnDef> GetColumnDefinitions(const UScriptStruct* RowStruct)
{
	TArray<FColumnDef> Columns;
	if (!RowStruct)
	{
		return Columns;
	}

	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		FProperty* Property = *It;
		FColumnDef Def;
		Def.Name = Property->GetName();
		Def.CppType = Property->GetCPPType();
		Def.Property = Property;
		Columns.Add(Def);
	}

	return Columns;
}

FString SerializeRowToText(const UDataTable* DataTable, FName RowName, const TArray<FString>* Columns)
{
	if (!DataTable)
	{
		return TEXT("Error: DataTable is null");
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (!RowStruct)
	{
		return TEXT("Error: Row struct is null");
	}

	const uint8* RowData = DataTable->FindRowUnchecked(RowName);
	if (!RowData)
	{
		return FString::Printf(TEXT("Error: Row '%s' not found"), *RowName.ToString());
	}

	FString Result;
	Result += FString::Printf(TEXT("Row: %s\n"), *RowName.ToString());

	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		const FProperty* Prop = *It;

		// Filter by columns if specified
		if (Columns && Columns->Num() > 0)
		{
			bool bFound = false;
			for (const FString& Col : *Columns)
			{
				if (Col.Equals(Prop->GetName(), ESearchCase::IgnoreCase))
				{
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				continue;
			}
		}

		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(RowData);
		FString ValueStr;
		Prop->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);
		Result += FString::Printf(TEXT("  %s: %s\n"), *Prop->GetName(), *ValueStr);
	}

	return Result;
}

FString GetPropertyValueAsString(const uint8* RowData, const FProperty* Property)
{
	if (!RowData || !Property)
	{
		return TEXT("?");
	}

	const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(RowData);
	FString ValueStr;
	Property->ExportText_Direct(ValueStr, ValuePtr, ValuePtr, nullptr, PPF_None);
	return ValueStr;
}

bool SetPropertyValueFromString(uint8* RowData, const FProperty* Property, const FString& Value, FString& OutError)
{
	if (!RowData || !Property)
	{
		OutError = TEXT("Invalid row data or property");
		return false;
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(RowData);
	const TCHAR* Result = Property->ImportText_Direct(*Value, ValuePtr, nullptr, PPF_None);
	if (!Result)
	{
		OutError = FString::Printf(TEXT("Failed to set property '%s' to '%s'"), *Property->GetName(), *Value);
		return false;
	}

	return true;
}

bool SetPropertyValues(UDataTable* DataTable, FName RowName, const TSharedPtr<FJsonObject>& Values, FString& OutError)
{
	if (!DataTable || !Values.IsValid())
	{
		OutError = TEXT("Invalid data table or values");
		return false;
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (!RowStruct)
	{
		OutError = TEXT("Row struct is null");
		return false;
	}

	uint8* RowData = DataTable->FindRowUnchecked(RowName);
	if (!RowData)
	{
		OutError = FString::Printf(TEXT("Row '%s' not found"), *RowName.ToString());
		return false;
	}

	for (const auto& Pair : Values->Values)
	{
		const FString& Key = Pair.Key;
		const TSharedPtr<FJsonValue>& JsonValue = Pair.Value;

		FProperty* Property = RowStruct->FindPropertyByName(FName(*Key));
		if (!Property)
		{
			OutError = FString::Printf(TEXT("Property '%s' not found in row struct '%s'"), *Key, *RowStruct->GetName());
			return false;
		}

		FString ValueString;
		if (!JsonValue->TryGetString(ValueString))
		{
			OutError = FString::Printf(TEXT("Value for property '%s' is not a string"), *Key);
			return false;
		}

		FString SetError;
		if (!SetPropertyValueFromString(RowData, Property, ValueString, SetError))
		{
			OutError = SetError;
			return false;
		}
	}

	return true;
}

} // namespace ClaireonDataTableHelpers
