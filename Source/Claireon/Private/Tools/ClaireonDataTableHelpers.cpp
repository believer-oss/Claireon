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
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"

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
	if (!IsValid(LoadedObj))
	{
		OutError = FString::Printf(TEXT("Failed to load asset at path: %s"), *ResolvedPath);
		return nullptr;
	}

	UDataTable* DataTable = Cast<UDataTable>(LoadedObj);
	if (!IsValid(DataTable))
	{
		OutError = FString::Printf(TEXT("Asset at %s is not a DataTable (actual type: %s)"), *ResolvedPath, *LoadedObj->GetClass()->GetName());
		return nullptr;
	}

	return DataTable;
}

bool IsCompositeDataTable(const UDataTable* DataTable)
{
	return IsValid(DataTable) && Cast<UCompositeDataTable>(DataTable) != nullptr;
}

bool EnsureWritable(const UDataTable* DataTable, FString& OutError)
{
	if (!IsValid(DataTable))
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
	if (!IsValid(DataTable))
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
	if (!IsValid(RowStruct))
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
	if (!IsValid(DataTable))
	{
		return TEXT("Error: DataTable is null");
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (!IsValid(RowStruct))
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
	if (!IsValid(DataTable) || !Values.IsValid())
	{
		OutError = TEXT("Invalid data table or values");
		return false;
	}

	const UScriptStruct* RowStruct = DataTable->GetRowStruct();
	if (!IsValid(RowStruct))
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
		const FString Key(*Pair.Key);
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

UCompositeDataTable* AsCompositeDataTable(UDataTable* DataTable)
{
	return Cast<UCompositeDataTable>(DataTable);
}

// ParentTables is protected; read it via its reflected UPROPERTY. Returns false
// (with OutError set) only when the property itself is missing — fail loudly.
static bool GetCompositeParentTables(const UCompositeDataTable* Composite, TArray<UDataTable*>& OutParents, FString& OutError)
{
	static const FName ParentTablesName(TEXT("ParentTables"));
	FArrayProperty* ParentsProp = CastField<FArrayProperty>(UCompositeDataTable::StaticClass()->FindPropertyByName(ParentTablesName));
	if (!ParentsProp)
	{
		OutError = TEXT("UCompositeDataTable::ParentTables property not found via reflection (engine layout changed?)");
		return false;
	}
	FObjectProperty* InnerProp = CastField<FObjectProperty>(ParentsProp->Inner);
	if (!InnerProp)
	{
		OutError = TEXT("UCompositeDataTable::ParentTables inner is not an object property (engine layout changed?)");
		return false;
	}

	FScriptArrayHelper ArrayHelper(ParentsProp, ParentsProp->ContainerPtrToValuePtr<void>(Composite));
	for (int32 i = 0; i < ArrayHelper.Num(); ++i)
	{
		UObject* Obj = InnerProp->GetObjectPropertyValue(ArrayHelper.GetRawPtr(i));
		if (UDataTable* Parent = Cast<UDataTable>(Obj); IsValid(Parent))
		{
			OutParents.Add(Parent);
		}
	}
	return true;
}

// DFS back-edge detection over composite parents only; true if the composite is reachable from itself.
static bool CompositeHasParentCycle(const UCompositeDataTable* Composite, TSet<const UCompositeDataTable*>& Visited, TSet<const UCompositeDataTable*>& InProgress)
{
	if (InProgress.Contains(Composite))
	{
		return true;
	}
	if (Visited.Contains(Composite))
	{
		return false;
	}
	InProgress.Add(Composite);
	TArray<UDataTable*> Parents;
	FString Ignored;
	GetCompositeParentTables(Composite, Parents, Ignored);
	for (const UDataTable* Parent : Parents)
	{
		if (const UCompositeDataTable* ParentComposite = Cast<UCompositeDataTable>(Parent); IsValid(ParentComposite))
		{
			if (CompositeHasParentCycle(ParentComposite, Visited, InProgress))
			{
				return true;
			}
		}
	}
	InProgress.Remove(Composite);
	Visited.Add(Composite);
	return false;
}

static bool CompositeHasParentCycle(const UCompositeDataTable* Composite)
{
	TSet<const UCompositeDataTable*> Visited;
	TSet<const UCompositeDataTable*> InProgress;
	return CompositeHasParentCycle(Composite, Visited, InProgress);
}

bool RefreshCompositeDataTable(UCompositeDataTable* Composite, FString& OutError)
{
	if (!IsValid(Composite))
	{
		OutError = TEXT("Composite data table is null");
		return false;
	}

	// A cyclic parent graph makes UpdateCachedRowMap->FindLoops open a blocking modal for an already-loaded composite, which hangs headless/CI.
	if (CompositeHasParentCycle(Composite))
	{
		OutError = TEXT("Composite has a cyclic parent-table graph; refusing to refresh (would hang on an editor modal)");
		return false;
	}

	// Empty append still fires OnParentTablesUpdated(ValueSet): rebuilds the cached
	// RowMap from current parents, re-subscribes delegates, and broadcasts OnDataTableChanged.
	Composite->AppendParentTables(TArray<UDataTable*>());
	// The rebuild only touches the transient cached RowMap, which leaves the package clean;
	// dirty it so SaveDataTable (bOnlyDirty) actually writes the refreshed cache to disk.
	Composite->MarkPackageDirty();
	return SaveDataTable(Composite, OutError);
}

TArray<FString> RefreshDependentComposites(const UDataTable* ModifiedTable, FString& OutError)
{
	TArray<FString> Refreshed;
	if (!IsValid(ModifiedTable))
	{
		OutError = TEXT("Modified data table is null");
		return Refreshed;
	}

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	// Wait so a dependent composite is never missed mid-scan in the headless export path (D2).
	AR.WaitForCompletion();

	// Load every composite once (bSearchSubClasses catches subclasses) and read its parents.
	// Registry dependency-edge queries (GetReferencers) are unreliable for freshly-saved
	// packages in-session, so inspect loaded ParentTables directly.
	TArray<FAssetData> CompositeAssets;
	AR.GetAssetsByClass(UCompositeDataTable::StaticClass()->GetClassPathName(), CompositeAssets, /*bSearchSubClasses=*/true);
	TArray<UCompositeDataTable*> AllComposites;
	TMap<UCompositeDataTable*, TArray<UDataTable*>> ParentsOf;
	for (const FAssetData& AD : CompositeAssets)
	{
		UCompositeDataTable* Composite = Cast<UCompositeDataTable>(AD.GetAsset());
		if (!IsValid(Composite))
		{
			continue;
		}
		TArray<UDataTable*> Parents;
		if (!GetCompositeParentTables(Composite, Parents, OutError))
		{
			// Reflection failure on the property is the silent-skip footgun — fail loudly.
			return TArray<FString>();
		}
		AllComposites.Add(Composite);
		ParentsOf.Add(Composite, MoveTemp(Parents));
	}

	// Dependents: composites listing ModifiedTable as a parent, transitively (frontier walk over reverse edges).
	TSet<UCompositeDataTable*> Dependents;
	TArray<const UDataTable*> Frontier;
	Frontier.Add(ModifiedTable);
	while (Frontier.Num() > 0)
	{
		const UDataTable* Table = Frontier.Pop(EAllowShrinking::No);
		for (UCompositeDataTable* Composite : AllComposites)
		{
			if (!Dependents.Contains(Composite) && ParentsOf[Composite].Contains(Table))
			{
				Dependents.Add(Composite);
				Frontier.Add(Composite);
			}
		}
	}

	// Single post-order DFS over dependent-parent edges yields deepest-first order (parents refresh before consumers); visited set is cycle-safe.
	TArray<UCompositeDataTable*> Order;
	TSet<UCompositeDataTable*> Visited;
	TFunction<void(UCompositeDataTable*)> Visit = [&](UCompositeDataTable* Composite)
	{
		if (Visited.Contains(Composite))
		{
			return;
		}
		Visited.Add(Composite);
		for (UDataTable* Parent : ParentsOf[Composite])
		{
			if (UCompositeDataTable* ParentComposite = Cast<UCompositeDataTable>(Parent); IsValid(ParentComposite))
			{
				if (Dependents.Contains(ParentComposite))
				{
					Visit(ParentComposite);
				}
			}
		}
		Order.Add(Composite);
	};
	for (UCompositeDataTable* Composite : Dependents)
	{
		Visit(Composite);
	}

	for (UCompositeDataTable* Composite : Order)
	{
		FString RefreshError;
		if (RefreshCompositeDataTable(Composite, RefreshError))
		{
			Refreshed.Add(Composite->GetPathName());
		}
		else
		{
			OutError += FString::Printf(TEXT("[%s] %s\n"), *Composite->GetPathName(), *RefreshError);
		}
	}

	return Refreshed;
}

FString RefreshDependentCompositesResult(UDataTable* Table, bool bRefreshComposites, const TSharedPtr<FJsonObject>& Data)
{
	TArray<FString> RefreshedComposites;
	FString RefreshErr;
	if (bRefreshComposites && IsValid(Table) && !IsCompositeDataTable(Table))
	{
		RefreshedComposites = RefreshDependentComposites(Table, RefreshErr);
	}

	if (Data.IsValid())
	{
		TArray<TSharedPtr<FJsonValue>> RefreshedArray;
		for (const FString& Path : RefreshedComposites)
		{
			RefreshedArray.Add(MakeShared<FJsonValueString>(Path));
		}
		Data->SetArrayField(TEXT("refreshed_composites"), RefreshedArray);
		if (!RefreshErr.IsEmpty())
		{
			Data->SetStringField(TEXT("refresh_warning"), RefreshErr);
		}
	}

	FString Suffix;
	if (RefreshedComposites.Num() > 0)
	{
		Suffix += FString::Printf(TEXT(" (refreshed %d composite(s))"), RefreshedComposites.Num());
	}
	if (!RefreshErr.IsEmpty())
	{
		Suffix += FString::Printf(TEXT("\nRefresh warning: %s"), *RefreshErr);
	}
	return Suffix;
}

TArray<UDataTable*> GetCompositeParentTableObjects(const UCompositeDataTable* Composite)
{
	TArray<UDataTable*> Parents;
	if (!IsValid(Composite))
	{
		return Parents;
	}

	FString ReflectionError;
	if (!GetCompositeParentTables(Composite, Parents, ReflectionError))
	{
		// Reflection failure is the silent-skip footgun -- return empty, callers see no parents rather than a wrong list.
		return TArray<UDataTable*>();
	}
	return Parents;
}

TArray<FString> GetCompositeParentTablePaths(const UCompositeDataTable* Composite)
{
	TArray<FString> Paths;
	const TArray<UDataTable*> Parents = GetCompositeParentTableObjects(Composite);
	Paths.Reserve(Parents.Num());
	for (const UDataTable* Parent : Parents)
	{
		if (IsValid(Parent))
		{
			Paths.Add(Parent->GetPathName());
		}
	}
	return Paths;
}

} // namespace ClaireonDataTableHelpers
