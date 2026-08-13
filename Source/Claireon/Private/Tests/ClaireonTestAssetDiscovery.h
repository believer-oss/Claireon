// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/Blueprint.h"
#include "Engine/CompositeDataTable.h"
#include "Engine/DataTable.h"
#include "Templates/Function.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

// Mirror-safe asset discovery for tests.
//
// Claireon ships no game content, so tests must not hardcode project asset paths --
// that both names project-specific content and only runs in the repo that has it.
// These helpers return an arbitrary suitable asset already present in the running
// project; call sites skip gracefully when none exists (e.g. a bare OSS host with
// no content).
namespace ClaireonTestAssetDiscovery
{
	inline IAssetRegistry* GetAssetRegistry()
	{
		FAssetRegistryModule* Mod = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry"));
		return Mod ? &Mod->Get() : nullptr;
	}

	// Only /Game content is usable as a fixture. The write tools these fixtures feed
	// (bp_duplicate, the datatable mutations) reject anything outside /Game outright --
	// picking /Engine/EngineDamageTypes/DmgTypeBP_Environmental, the first UBlueprint the
	// registry happens to return, made the duplicate acceptance case fail with
	// "Invalid asset path". /Game/__MCPTests is this suite's own scratch space and must
	// never be mistaken for project content.
	inline bool IsUsableFixturePackage(const FString& PackageName)
	{
		return PackageName.StartsWith(TEXT("/Game/"))
			&& !PackageName.StartsWith(TEXT("/Game/__MCPTests"));
	}

	// First usable asset of exactly ExactClass (subclasses excluded), object path or empty.
	inline FString FindProjectAssetObjectPath(const UClass* ExactClass)
	{
		IAssetRegistry* AR = GetAssetRegistry();
		if (!AR || ExactClass == nullptr)
		{
			return FString();
		}
		TArray<FAssetData> Assets;
		AR->GetAssetsByClass(ExactClass->GetClassPathName(), Assets, /*bSearchSubClasses*/ false);
		for (const FAssetData& A : Assets)
		{
			if (!IsUsableFixturePackage(A.PackageName.ToString()))
			{
				continue;
			}
			return A.GetSoftObjectPath().ToString();
		}
		return FString();
	}

	// A plain UDataTable (composite excluded). Object path, or empty if none.
	inline FString FindProjectDataTableObjectPath()
	{
		return FindProjectAssetObjectPath(UDataTable::StaticClass());
	}

	// A UCompositeDataTable. Object path, or empty if none.
	inline FString FindProjectCompositeDataTableObjectPath()
	{
		return FindProjectAssetObjectPath(UCompositeDataTable::StaticClass());
	}

	// Upper bound on how many candidate tables may be LOADED by one scan. Loading is the
	// expensive and risky part: every extra asset resident in memory widens the
	// GetAllReferencersIncludingWeak scan that runs on each asset delete, which is the
	// known referencer-scan crash this suite already trips on. The tag-based filter below
	// keeps the normal case at one load or zero, and this caps the pathological case.
	inline constexpr int32 MaxDataTablesToLoad = 16;

	// Row struct of a data table read from the asset registry, WITHOUT loading the table.
	// UDataTable publishes its row struct's path name under the "RowStructure" tag
	// (Engine/Private/DataTable.cpp). Returns null when the tag is absent (older or
	// unsaved assets) -- callers fall back to loading.
	inline const UScriptStruct* FindRowStructFromAssetTag(const FAssetData& AssetData)
	{
		static const FName RowStructureTag(TEXT("RowStructure"));
		FString StructPath;
		if (!AssetData.GetTagValue(RowStructureTag, StructPath) || StructPath.IsEmpty())
		{
			return nullptr;
		}
		if (UScriptStruct* AlreadyLoaded = UClass::TryFindTypeSlow<UScriptStruct>(StructPath))
		{
			return AlreadyLoaded;
		}
		// Blueprint user-defined structs live in their own (small) package.
		return LoadObject<UScriptStruct>(nullptr, *StructPath);
	}

	// First plain UDataTable with at least MinRows rows whose row struct satisfies
	// RowStructPredicate. Object path, or empty if none matches.
	//
	// The row struct is tested from the registry tag first, so tables whose shape is wrong
	// are rejected without ever being loaded. Row count has no registry tag, so a table
	// that passes the struct predicate is loaded only when MinRows > 1 demands it.
	inline FString FindProjectDataTableObjectPathMatching(
		TFunctionRef<bool(const UScriptStruct*)> RowStructPredicate,
		int32 MinRows)
	{
		IAssetRegistry* AR = GetAssetRegistry();
		if (!AR)
		{
			return FString();
		}
		TArray<FAssetData> Assets;
		AR->GetAssetsByClass(UDataTable::StaticClass()->GetClassPathName(), Assets, /*bSearchSubClasses*/ false);
		int32 Loaded = 0;
		for (const FAssetData& A : Assets)
		{
			if (!IsUsableFixturePackage(A.PackageName.ToString()))
			{
				continue;
			}

			const UScriptStruct* TaggedRowStruct = FindRowStructFromAssetTag(A);
			if (TaggedRowStruct && !RowStructPredicate(TaggedRowStruct))
			{
				continue;
			}
			if (TaggedRowStruct && MinRows <= 1)
			{
				return A.GetSoftObjectPath().ToString();
			}

			// Either the row count still has to be confirmed, or the tag was missing and the
			// struct itself is only readable from the loaded table.
			if (++Loaded > MaxDataTablesToLoad)
			{
				break;
			}
			const UDataTable* DT = Cast<UDataTable>(A.GetSoftObjectPath().TryLoad());
			if (!IsValid(DT) || !IsValid(DT->GetRowStruct()) || DT->GetRowMap().Num() < MinRows)
			{
				continue;
			}
			if (RowStructPredicate(DT->GetRowStruct()))
			{
				return A.GetSoftObjectPath().ToString();
			}
		}
		return FString();
	}

	// True if RowStruct has at least one property for which Predicate holds.
	inline bool RowStructHasProperty(const UScriptStruct* RowStruct, TFunctionRef<bool(const FProperty*)> Predicate)
	{
		for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
		{
			if (Predicate(*It))
			{
				return true;
			}
		}
		return false;
	}

	// A multi-row table whose row struct carries at least one scalar column. This is the
	// general-purpose read fixture: the read/export tests need >= 2 rows (pagination
	// asserts "showing rows 1-2") and at least one bool/number/string/enum column to
	// type-check against. Object path, or empty if none.
	inline FString FindProjectDataTableWithScalarColumnObjectPath()
	{
		return FindProjectDataTableObjectPathMatching(
			[](const UScriptStruct* RowStruct)
			{
				return RowStructHasProperty(RowStruct, [](const FProperty* P)
				{
					return CastField<FBoolProperty>(P) || CastField<FIntProperty>(P) || CastField<FInt64Property>(P)
						|| CastField<FFloatProperty>(P) || CastField<FDoubleProperty>(P)
						|| CastField<FStrProperty>(P) || CastField<FNameProperty>(P) || CastField<FEnumProperty>(P);
				});
			},
			/*MinRows*/ 2);
	}

	// A table whose row struct carries at least one FStructProperty column (needed by the
	// include_schema round-trip, which only compares schema on struct columns). Object
	// path, or empty if none.
	inline FString FindProjectDataTableWithStructColumnObjectPath()
	{
		return FindProjectDataTableObjectPathMatching(
			[](const UScriptStruct* RowStruct)
			{
				return RowStructHasProperty(RowStruct, [](const FProperty* P)
				{
					return CastField<FStructProperty>(P) != nullptr;
				});
			},
			/*MinRows*/ 1);
	}

	// A table whose row struct carries at least one FMapProperty column (TMap surfaces as
	// a JSON array of { key, value } in structured row output). Object path, or empty.
	inline FString FindProjectDataTableWithMapColumnObjectPath()
	{
		return FindProjectDataTableObjectPathMatching(
			[](const UScriptStruct* RowStruct)
			{
				return RowStructHasProperty(RowStruct, [](const FProperty* P)
				{
					return CastField<FMapProperty>(P) != nullptr;
				});
			},
			/*MinRows*/ 1);
	}

	// An on-disk UBlueprint (exact class; Anim/Widget/etc. excluded). Object path, or empty.
	inline FString FindProjectBlueprintObjectPath()
	{
		return FindProjectAssetObjectPath(UBlueprint::StaticClass());
	}
}
