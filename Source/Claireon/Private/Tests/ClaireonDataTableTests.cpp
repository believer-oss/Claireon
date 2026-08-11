// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonTool_DataTableSearch.h"
#include "Tools/ClaireonTool_DataTableGetInfo.h"
#include "Tools/ClaireonTool_DataTableGetRows.h"
#include "Tools/ClaireonTool_DataTableGetRowStructured.h"
#include "Tools/ClaireonTool_DataTableFindRows.h"
#include "Tools/ClaireonTool_DataTableAddRow.h"
#include "Tools/ClaireonTool_DataTableRemoveRow.h"
#include "Tools/ClaireonTool_DataTableDuplicateRow.h"
#include "Tools/ClaireonTool_DataTableRenameRow.h"
#include "Tools/ClaireonTool_DataTableMoveRow.h"
#include "Tools/ClaireonTool_DataTableSetRowValues.h"
#include "Tools/ClaireonTool_DataTableExportJson.h"
#include "Tools/ClaireonTool_DataTableImportJson.h"
#include "Tools/ClaireonTool_DataTableExportCsv.h"
#include "Tools/ClaireonTool_DataTableImportCsv.h"
#include "Tools/ClaireonDataTableHelpers.h"
#include "ClaireonStructReflection.h"
#include "ClaireonTestAssetDiscovery.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Engine/DataTable.h"
#include "Engine/CompositeDataTable.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

#include "ClaireonTestAssetDeletion.h"
// ---------------------------------------------------------------------------
// Test asset paths
//
// Claireon ships no content of its own, so the read fixtures are DISCOVERED from
// whatever the host project already has (see ClaireonTestAssetDiscovery.h) rather
// than hardcoded: a hardcoded path both names project-specific content and only
// resolves in the one repo that has it. Each accessor caches its first lookup, so
// every test in a run sees the same table.
//
// SourceDTPath() is READ-ONLY project content. Every datatable mutation tool
// (add / remove / duplicate / rename / move / set_row_values / import_*) calls
// ClaireonDataTableHelpers::SaveDataTable, which writes the package to disk.
// Pointing the mutation lifecycle test at the discovered table therefore left that
// .uasset modified in git after every full suite run -- and since the test PASSES,
// failure triage never surfaced it. The only detector is
// `git status --porcelain -- Content/` being non-empty after a run; it must stay
// empty. Mutating tests work on MutableDTPath instead.
// ---------------------------------------------------------------------------

namespace ClaireonDataTableTestPaths
{
	// Package path (/Game/Foo/DT_Bar) for a discovered object path, or empty.
	// The tools take either form; package paths keep the assertions readable.
	static FString ToPackagePath(const FString& ObjectPath)
	{
		return ObjectPath.IsEmpty() ? FString() : FPackageName::ObjectPathToPackageName(ObjectPath);
	}

	static FString ShortName(const FString& PackagePath)
	{
		return PackagePath.IsEmpty() ? FString() : FPackageName::GetShortName(PackagePath);
	}
}

/** Multi-row read fixture with at least one scalar column. Empty if the project has none. */
static const FString& SourceDTPath()
{
	static const FString Cached = ClaireonDataTableTestPaths::ToPackagePath(
		ClaireonTestAssetDiscovery::FindProjectDataTableWithScalarColumnObjectPath());
	return Cached;
}

/** Asset name of SourceDTPath(), for assertions on name-bearing output. */
static const FString& SourceDTName()
{
	static const FString Cached = ClaireonDataTableTestPaths::ShortName(SourceDTPath());
	return Cached;
}

/** Composite fixture for the write-rejection test. Empty if the project has none. */
static const FString& TestCompositeDTPath()
{
	static const FString Cached = ClaireonDataTableTestPaths::ToPackagePath(
		ClaireonTestAssetDiscovery::FindProjectCompositeDataTableObjectPath());
	return Cached;
}

/** Fixture with a TMap column, for the structured map-emission test. Empty if none. */
static const FString& TestMapColumnDTPath()
{
	static const FString Cached = ClaireonDataTableTestPaths::ToPackagePath(
		ClaireonTestAssetDiscovery::FindProjectDataTableWithMapColumnObjectPath());
	return Cached;
}

/** Fixture with an FStructProperty column, for the include_schema round-trip. Empty if none. */
static const FString& TestStructColumnDTPath()
{
	static const FString Cached = ClaireonDataTableTestPaths::ToPackagePath(
		ClaireonTestAssetDiscovery::FindProjectDataTableWithStructColumnObjectPath());
	return Cached;
}

static const TCHAR* MutableDTPath = TEXT("/Game/__MCPTests/DT_MutationTest");
static const TCHAR* TestBadDTPath = TEXT("/Game/DoesNotExist/DT_Fake");

namespace ClaireonDataTableTestsFixtures
{
	// Duplicate SourcePath -> DestPath, deleting any pre-existing DestPath first.
	//
	// The leading delete is required, not defensive: /Game/__MCPTests is NOT gitignored
	// and PERSISTS between runs, so a run that dies mid-test would otherwise hand a
	// mutated fixture (stray _UNTEST_ rows and all) to the next run.
	static bool EnsureFreshDuplicate(const FString& SourcePath, const FString& DestPath)
	{
		if (SourcePath.IsEmpty())
		{
			return false;
		}
		if (UEditorAssetLibrary::DoesAssetExist(DestPath))
		{
			ClaireonTestAssetDeletion::DeleteAssetForTest(DestPath);
		}
		return UEditorAssetLibrary::DuplicateAsset(SourcePath, DestPath) != nullptr;
	}

	static void DeleteDuplicate(const FString& Path)
	{
		if (UEditorAssetLibrary::DoesAssetExist(Path))
		{
			ClaireonTestAssetDeletion::DeleteAssetForTest(Path);
		}
	}
}

// Unique prefix for rows created by mutation tests — prevents collisions
static const TCHAR* UntestTempRowA = TEXT("_UNTEST_TempRow_A");
static const TCHAR* UntestTempRowB = TEXT("_UNTEST_TempRow_B");
static const TCHAR* UntestTempRowC = TEXT("_UNTEST_TempRow_C");

// ============================================================================
// Schema validation — Read tools
// ============================================================================

// NOTE on the TryGetArrayField/TryGetObjectField calls below: they are
// UNTEST_ASSERT_*, not UNTEST_EXPECT_*, on purpose. EXPECT records the failure and
// CONTINUES, so a schema that lost its "required" array would fall through to
// `Required->Num()` and dereference an UNINITIALIZED pointer -- a crash that takes
// the whole run out instead of a reported failure. Any out-param probe whose result
// is dereferenced on the next line must be an ASSERT.
UNTEST_UNIT_OPTS(Claireon, DataTableSchema, ReadToolsValid, UNTEST_TIMEOUTMS(10000))
{
	// datatable_search
	{
		ClaireonTool_DataTableSearch Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_search"));
		UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		FString Type;
		UNTEST_EXPECT_TRUE(Schema->TryGetStringField(TEXT("type"), Type));
		UNTEST_EXPECT_STREQ(Type, TEXT("object"));
		const TSharedPtr<FJsonObject>* Props = nullptr;
		UNTEST_EXPECT_TRUE(Schema->TryGetObjectField(TEXT("properties"), Props));
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 1);
	}

	// get_datatable_info
	{
		ClaireonTool_DataTableGetInfo Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_get_info"));
		UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 1);
	}

	// get_datatable_rows
	{
		ClaireonTool_DataTableGetRows Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_get_rows"));
		UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
	}

	// datatable_get_row (formerly get_row_structured -- now the primary get_row tool)
	{
		ClaireonTool_DataTableGetRowStructured Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_get_row"));
		UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 2);

		// asset_path + row_name both required.
		bool bAssetPathRequired = false;
		bool bRowNameRequired = false;
		for (const TSharedPtr<FJsonValue>& V : *Required)
		{
			FString S;
			if (V.IsValid() && V->TryGetString(S))
			{
				if (S == TEXT("asset_path")) bAssetPathRequired = true;
				if (S == TEXT("row_name")) bRowNameRequired = true;
			}
		}
		UNTEST_EXPECT_TRUE(bAssetPathRequired);
		UNTEST_EXPECT_TRUE(bRowNameRequired);

		// columns and include_schema present as optional properties.
		const TSharedPtr<FJsonObject>* PropsObj = nullptr;
		UNTEST_EXPECT_TRUE(Schema->TryGetObjectField(TEXT("properties"), PropsObj));
		if (PropsObj && PropsObj->IsValid())
		{
			UNTEST_EXPECT_TRUE((*PropsObj)->HasField(TEXT("columns")));
			UNTEST_EXPECT_TRUE((*PropsObj)->HasField(TEXT("include_schema")));
		}
	}

	// find_datatable_rows
	{
		ClaireonTool_DataTableFindRows Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_find_rows"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 3);
	}

	co_return;
}

// ============================================================================
// Schema validation — Mutation tools
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTableSchema, MutationToolsValid, UNTEST_TIMEOUTMS(10000))
{
	// add_datatable_row
	{
		ClaireonTool_DataTableAddRow Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_add_row"));
		UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 2);
	}

	// remove_datatable_row
	{
		ClaireonTool_DataTableRemoveRow Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_remove_row"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
	}

	// datatable_duplicate_row
	{
		ClaireonTool_DataTableDuplicateRow Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_duplicate_row"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 3);
	}

	// datatable_rename_row
	{
		ClaireonTool_DataTableRenameRow Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_rename_row"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 3);
	}

	// datatable_move_row
	{
		ClaireonTool_DataTableMoveRow Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_move_row"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 3);
	}

	// set_datatable_row
	{
		ClaireonTool_DataTableSetRowValues Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_set_row_values"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 3);
	}

	co_return;
}

// ============================================================================
// Schema validation — Import/Export tools
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTableSchema, ImportExportToolsValid, UNTEST_TIMEOUTMS(10000))
{
	// datatable_export_json
	{
		ClaireonTool_DataTableExportJson Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_export_json"));
		UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
	}

	// datatable_import_json
	{
		ClaireonTool_DataTableImportJson Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_import_json"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 2);
	}

	// datatable_export_csv
	{
		ClaireonTool_DataTableExportCsv Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_export_csv"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
	}

	// datatable_import_csv
	{
		ClaireonTool_DataTableImportCsv Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_import_csv"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_ASSERT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 2);
	}

	co_return;
}

// ============================================================================
// Error handling — Missing required parameters
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTable, SearchMissingQuery, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_DataTableSearch Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("query")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, GetInfoMissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_DataTableGetInfo Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("asset_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, GetRowMissingParams, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_DataTableGetRowStructured Tool;

	// Missing both params
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("asset_path")));
	}

	// Missing row_name
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), SourceDTPath());
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("row_name")));
	}

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, FindRowsMissingParams, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_DataTableFindRows Tool;

	// Missing all params
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_TRUE(Result.bIsError);
	}

	// Missing column and value
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), SourceDTPath());
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("column")));
	}

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, AddRowMissingRowName, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_DataTableAddRow Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourceDTPath());
	// Missing row_name
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("row_name")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, SetRowValuesMissingValues, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_DataTableSetRowValues Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourceDTPath());
	Args->SetStringField(TEXT("row_name"), TEXT("SomeRow"));
	// Missing values object
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("values")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, MoveRowInvalidDirection, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_DataTableMoveRow Tool;

	// Missing direction
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), SourceDTPath());
		Args->SetStringField(TEXT("row_name"), TEXT("SomeRow"));
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("direction")));
	}

	// Invalid direction value
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), SourceDTPath());
		Args->SetStringField(TEXT("row_name"), TEXT("SomeRow"));
		Args->SetStringField(TEXT("direction"), TEXT("sideways"));
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Invalid direction")));
	}

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, ImportJsonMissingJson, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_DataTableImportJson Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourceDTPath());
	// Missing json
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("json")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, ImportCsvMissingCsv, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_DataTableImportCsv Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourceDTPath());
	// Missing csv
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("csv")));
	co_return;
}

// ============================================================================
// Error handling — Bad asset paths
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTable, GetInfoBadAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_DataTableGetInfo Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestBadDTPath);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Failed to load")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, AddRowBadAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_DataTableAddRow Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestBadDTPath);
	Args->SetStringField(TEXT("row_name"), TEXT("TestRow"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Failed to load")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, GetRowNonexistentRow, UNTEST_TIMEOUTMS(30000))
{
	ClaireonTool_DataTableGetRowStructured Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourceDTPath());
	Args->SetStringField(TEXT("row_name"), TEXT("_UNTEST_NonexistentRow_999"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("not found")));
	co_return;
}

// ============================================================================
// Error handling — datatable_get_row (structured)
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTable, GetRowStructuredErrors, UNTEST_TIMEOUTMS(30000))
{
	ClaireonTool_DataTableGetRowStructured Tool;

	// Missing asset_path
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("asset_path")));
	}

	// Missing row_name
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), SourceDTPath());
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("row_name")));
	}

	// Nonexistent asset path
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), TestBadDTPath);
		Args->SetStringField(TEXT("row_name"), TEXT("AnyRow"));
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_TRUE(Result.bIsError);
		// LoadDataTableAsset returns "Failed to load" style errors
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Failed to load")));
	}

	// Asset path points to a non-DataTable (use a known engine asset)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), TEXT("/Engine/EngineMaterials/DefaultMaterial"));
		Args->SetStringField(TEXT("row_name"), TEXT("AnyRow"));
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_TRUE(Result.bIsError);
		// Either "Failed to load" (not loadable as UDataTable) or similar -- the bridge guarantees an error.
		UNTEST_EXPECT_FALSE(Result.GetContentAsString().IsEmpty());
	}

	// Valid asset, nonexistent row name
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), SourceDTPath());
		Args->SetStringField(TEXT("row_name"), TEXT("_UNTEST_NonexistentRow_999"));
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("_UNTEST_NonexistentRow_999")));
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("not found")));
	}

	co_return;
}

// ============================================================================
// Read operations — Valid asset
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTable, SearchFindsKnownTable, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_DataTableSearch Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	UNTEST_ASSERT_FALSE(SourceDTName().IsEmpty());
	Args->SetStringField(TEXT("query"), SourceDTName());
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	FString Output = Result.GetContentAsString();
	// Root cause: the zero-results path is MakeSuccessResult with the message
	// "No data tables found matching \"<query>\"" (ClaireonTool_DataTableSearch.cpp).
	// That message ECHOES the query, so Contains(<query>) passed, and "Found"
	// matched "...found matching..." case-insensitively -- every assertion in this
	// test passed on the tool's primary failure mode. The full package path only
	// appears in the numbered results listing, so assert that instead.
	UNTEST_EXPECT_TRUE(Output.Contains(SourceDTPath(), ESearchCase::CaseSensitive));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("data table(s) matching"), ESearchCase::CaseSensitive));
	UNTEST_EXPECT_FALSE(Output.Contains(TEXT("No data tables found"), ESearchCase::CaseSensitive));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, GetInfoReturnsMetadata, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_DataTableGetInfo Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourceDTPath());
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Structured data fields
	FString TablePath;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("table_path"), TablePath));
	UNTEST_EXPECT_TRUE(TablePath.Contains(SourceDTName()));

	FString RowStruct;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("row_struct"), RowStruct));
	UNTEST_EXPECT_TRUE(!RowStruct.IsEmpty());

	// Root cause: this used to be RowCount >= 0.0 on a value that cannot be
	// negative, i.e. an assertion no input could fail. Compare against the table
	// itself instead, and require a non-empty table so the columns[] walk below
	// cannot be vacuous either.
	FString LoadError;
	UDataTable* DT = ClaireonDataTableHelpers::LoadDataTableAsset(SourceDTPath(), LoadError);
	UNTEST_ASSERT_PTR(DT);
	const int32 ExpectedRowCount = DT->GetRowMap().Num();
	UNTEST_ASSERT_GT(ExpectedRowCount, 0);
	UNTEST_EXPECT_STREQ(RowStruct, DT->GetRowStruct() ? DT->GetRowStruct()->GetName() : FString());

	double RowCount = 0.0;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetNumberField(TEXT("row_count"), RowCount));
	UNTEST_EXPECT_EQ((int32)RowCount, ExpectedRowCount);

	bool bIsComposite = false;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetBoolField(TEXT("is_composite"), bIsComposite));
	UNTEST_EXPECT_FALSE(bIsComposite);

	// columns[] contents were never inspected: one entry per row-struct property,
	// each carrying a non-empty name and cpp type.
	const TArray<TSharedPtr<FJsonValue>>* Columns = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("columns"), Columns));
	int32 ExpectedColumnCount = 0;
	for (TFieldIterator<FProperty> It(DT->GetRowStruct()); It; ++It) { ++ExpectedColumnCount; }
	UNTEST_EXPECT_EQ(Columns->Num(), ExpectedColumnCount);
	for (const TSharedPtr<FJsonValue>& ColVal : *Columns)
	{
		UNTEST_ASSERT_TRUE(ColVal.IsValid() && ColVal->Type == EJson::Object);
		const TSharedPtr<FJsonObject> ColObj = ColVal->AsObject();
		UNTEST_ASSERT_TRUE(ColObj.IsValid());
		FString ColName, ColType;
		UNTEST_EXPECT_TRUE(ColObj->TryGetStringField(TEXT("name"), ColName));
		UNTEST_EXPECT_TRUE(ColObj->TryGetStringField(TEXT("type"), ColType));
		UNTEST_EXPECT_FALSE(ColName.IsEmpty());
		UNTEST_EXPECT_FALSE(ColType.IsEmpty());
	}

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, GetRowsReturnsList, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_DataTableGetRows Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourceDTPath());
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Root cause: this used to assert ReturnedRows >= 0.0 (unfalsifiable for a count)
	// and never looked inside rows[]. Tie the counters to the table and to the array
	// they describe, and check the row payloads.
	FString LoadError;
	UDataTable* DT = ClaireonDataTableHelpers::LoadDataTableAsset(SourceDTPath(), LoadError);
	UNTEST_ASSERT_PTR(DT);
	const int32 ExpectedTotal = DT->GetRowMap().Num();
	UNTEST_ASSERT_GT(ExpectedTotal, 0);

	double TotalRows = 0.0;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetNumberField(TEXT("total_rows"), TotalRows));
	UNTEST_EXPECT_EQ((int32)TotalRows, ExpectedTotal);

	double ReturnedRows = 0.0;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetNumberField(TEXT("returned_rows"), ReturnedRows));

	const TArray<TSharedPtr<FJsonValue>>* RowsArray = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("rows"), RowsArray));
	// returned_rows must describe rows[], not an unrelated counter.
	UNTEST_EXPECT_EQ(RowsArray->Num(), (int32)ReturnedRows);
	UNTEST_ASSERT_GT(RowsArray->Num(), 0);

	// Every emitted row must name a row that actually exists in the table.
	for (const TSharedPtr<FJsonValue>& RowVal : *RowsArray)
	{
		UNTEST_ASSERT_TRUE(RowVal.IsValid() && RowVal->Type == EJson::Object);
		const TSharedPtr<FJsonObject> RowObj = RowVal->AsObject();
		UNTEST_ASSERT_TRUE(RowObj.IsValid());
		FString RowName;
		UNTEST_EXPECT_TRUE(RowObj->TryGetStringField(TEXT("row_name"), RowName));
		UNTEST_EXPECT_TRUE(DT->GetRowMap().Contains(FName(*RowName)));
	}

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, GetRowsPagination, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_DataTableGetRows Tool;

	// Request only 2 rows with offset
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourceDTPath());
	Args->SetNumberField(TEXT("max_rows"), 2);
	Args->SetNumberField(TEXT("offset"), 0);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Summary contains pagination info like "<TableName>: showing rows 1-2 of N"
	FString Summary = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Summary.Contains(TEXT("showing rows 1-2")));

	// Structured data: returned_rows should be at most 2
	double ReturnedRows = 0.0;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetNumberField(TEXT("returned_rows"), ReturnedRows));
	UNTEST_EXPECT_TRUE(ReturnedRows <= 2.0);

	co_return;
}

// ============================================================================
// Export operations
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTable, ExportJsonValid, UNTEST_TIMEOUTMS(30000))
{
	ClaireonTool_DataTableExportJson Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourceDTPath());
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	FString Output = Result.GetContentAsString();
	// Root cause: Contains("[") / Contains("]") was the SOLE coverage of this tool
	// and cannot fail for any string that reaches it (the summary line of an error
	// envelope would satisfy it too). Parse the payload and check it against the
	// table: one JSON object per row, each naming a real row.
	FString LoadError;
	UDataTable* DT = ClaireonDataTableHelpers::LoadDataTableAsset(SourceDTPath(), LoadError);
	UNTEST_ASSERT_PTR(DT);
	UNTEST_ASSERT_GT(DT->GetRowMap().Num(), 0);

	TArray<TSharedPtr<FJsonValue>> Parsed;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Output);
	UNTEST_ASSERT_TRUE(FJsonSerializer::Deserialize(Reader, Parsed));
	UNTEST_EXPECT_EQ(Parsed.Num(), DT->GetRowMap().Num());

	for (const TSharedPtr<FJsonValue>& RowVal : Parsed)
	{
		UNTEST_ASSERT_TRUE(RowVal.IsValid() && RowVal->Type == EJson::Object);
		const TSharedPtr<FJsonObject> RowObj = RowVal->AsObject();
		UNTEST_ASSERT_TRUE(RowObj.IsValid());
		UNTEST_EXPECT_GT(RowObj->Values.Num(), 0);
	}

	// Every row name must appear in the export. Asserted against the text rather
	// than a fixed key field because the key field name is table-configurable
	// (UDataTable::ImportKeyField, defaulting to "Name").
	for (const TPair<FName, uint8*>& RowPair : DT->GetRowMap())
	{
		UNTEST_EXPECT_TRUE(Output.Contains(RowPair.Key.ToString(), ESearchCase::CaseSensitive));
	}

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, ExportCsvValid, UNTEST_TIMEOUTMS(30000))
{
	ClaireonTool_DataTableExportCsv Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourceDTPath());
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	FString Output = Result.GetContentAsString();
	// Root cause: Output.Len() > 10 was the SOLE coverage of this tool and cannot
	// fail for any output that reaches it -- even a one-line error summary. Check the
	// actual CSV shape against the table: a header naming every column, then one line
	// per row, and every row name present.
	FString LoadError;
	UDataTable* DT = ClaireonDataTableHelpers::LoadDataTableAsset(SourceDTPath(), LoadError);
	UNTEST_ASSERT_PTR(DT);
	const UScriptStruct* RowStruct = DT->GetRowStruct();
	UNTEST_ASSERT_PTR(RowStruct);
	UNTEST_ASSERT_GT(DT->GetRowMap().Num(), 0);

	TArray<FString> Lines;
	Output.ParseIntoArrayLines(Lines, /*bCullEmpty=*/true);
	// Header plus at least one line per row. Not an equality check: a multi-line
	// FText cell (Description carries meta=(MultiLine)) can legitimately wrap.
	UNTEST_ASSERT_GE(Lines.Num(), DT->GetRowMap().Num() + 1);

	const FString& Header = Lines[0];
	UNTEST_EXPECT_TRUE(Header.Contains(TEXT(","), ESearchCase::CaseSensitive));
	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		UNTEST_EXPECT_TRUE(Header.Contains(It->GetName(), ESearchCase::CaseSensitive));
	}

	// Each row must start its own CSV line (row name is the first field).
	for (const TPair<FName, uint8*>& RowPair : DT->GetRowMap())
	{
		const FString RowName = RowPair.Key.ToString();
		bool bFoundLine = false;
		for (int32 LineIdx = 1; LineIdx < Lines.Num(); ++LineIdx)
		{
			if (Lines[LineIdx].StartsWith(RowName, ESearchCase::CaseSensitive))
			{
				bFoundLine = true;
				break;
			}
		}
		UNTEST_EXPECT_TRUE(bFoundLine);
	}

	co_return;
}

// ============================================================================
// Import operations -- SUCCESS PATH
//
// datatable_import_json and datatable_import_csv had no successful-path coverage
// at all: only ImportJsonMissingJson / ImportCsvMissingCsv (argument validation)
// and the composite-rejection branch. Both tools could have silently stopped
// writing rows and the suite would have stayed green.
//
// Both tests below work on a duplicate of the discovered source table in
// /Game/__MCPTests, never on the project's own table: every import call ends in
// ClaireonDataTableHelpers::SaveDataTable, which writes the package to disk. An
// in-memory-only fixture is not an option for the same reason -- the tools save
// unconditionally -- so this follows the established EnsureFreshDuplicate +
// ON_SCOPE_EXIT DeleteDuplicate pattern that MutationAddAndRemoveLifecycle uses,
// and `git status --porcelain -- Content/` must still be empty after a run.
//
// Payloads are derived from the table's OWN export rather than hard-coded, so
// neither test has to know the fixture's row struct and neither breaks when that
// struct gains a column.
//
// Verification reads UDataTable::GetRowMap() directly. It deliberately does not
// go through the tool's Summary: import_json/import_csv report their row count
// only in the prose ("Imported N rows."), and Result.Data carries just the
// composite-refresh block -- so the prose is not a substitute for looking at the
// table (see ClaireonTestDataAssertions.h on the Data-vs-Summary split).
// ============================================================================

namespace ClaireonDataTableImportSuccessTests
{
	/** Row-name set of a table loaded by path. Empty on load failure. */
	static TSet<FName> ImportTest_RowNames(const FString& AssetPath)
	{
		TSet<FName> Names;
		FString LoadError;
		UDataTable* DT = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, LoadError);
		if (!IsValid(DT))
		{
			return Names;
		}
		for (const TPair<FName, uint8*>& Pair : DT->GetRowMap())
		{
			Names.Add(Pair.Key);
		}
		return Names;
	}

	static FString ImportTest_ExportJson(const FString& AssetPath)
	{
		ClaireonTool_DataTableExportJson Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		const IClaireonTool::FToolResult Result = Tool.Execute(Args);
		return Result.bIsError ? FString() : Result.Summary;
	}

	static FString ImportTest_ExportCsv(const FString& AssetPath)
	{
		ClaireonTool_DataTableExportCsv Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		const IClaireonTool::FToolResult Result = Tool.Execute(Args);
		return Result.bIsError ? FString() : Result.Summary;
	}

	static IClaireonTool::FToolResult ImportTest_ImportJson(const FString& AssetPath, const FString& Json)
	{
		ClaireonTool_DataTableImportJson Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("json"), Json);
		// The fixture is a standalone duplicate, so nothing aggregates it.
		Args->SetBoolField(TEXT("refresh_composites"), false);
		return Tool.Execute(Args);
	}

	static IClaireonTool::FToolResult ImportTest_ImportCsv(const FString& AssetPath, const FString& Csv)
	{
		ClaireonTool_DataTableImportCsv Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("csv"), Csv);
		Args->SetBoolField(TEXT("refresh_composites"), false);
		return Tool.Execute(Args);
	}
}

static const TCHAR* ImportJsonDTPath = TEXT("/Game/__MCPTests/DT_ImportJsonSuccessTest");
static const TCHAR* ImportCsvDTPath  = TEXT("/Game/__MCPTests/DT_ImportCsvSuccessTest");

UNTEST_UNIT_OPTS(Claireon, DataTable, ImportJsonReplacesTableContents, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonDataTableImportSuccessTests;

	// The source table was discovered by loading it, so a failed duplicate is a real
	// problem -- fail rather than warn-and-co_return (which Untest scores as a PASS).
	UNTEST_ASSERT_FALSE(SourceDTPath().IsEmpty());
	const bool bDuplicated =
		ClaireonDataTableTestsFixtures::EnsureFreshDuplicate(SourceDTPath(), ImportJsonDTPath);
	ON_SCOPE_EXIT { ClaireonDataTableTestsFixtures::DeleteDuplicate(ImportJsonDTPath); };
	if (!bDuplicated)
	{
		UE_LOG(LogTemp, Error, TEXT("[Claireon.DataTable] Could not duplicate fixture %s -> %s"),
			*SourceDTPath(), ImportJsonDTPath);
	}
	UNTEST_ASSERT_TRUE(bDuplicated);

	// Baseline: the full export is the round-trip payload, and the row-name set is
	// what a successful restore has to reproduce.
	const TSet<FName> OriginalRows = ImportTest_RowNames(ImportJsonDTPath);
	UNTEST_ASSERT_GT(OriginalRows.Num(), 1);

	const FString JsonAll = ImportTest_ExportJson(ImportJsonDTPath);
	UNTEST_ASSERT_FALSE(JsonAll.IsEmpty());

	TArray<TSharedPtr<FJsonValue>> AllRowValues;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonAll);
		UNTEST_ASSERT_TRUE(FJsonSerializer::Deserialize(Reader, AllRowValues));
	}
	UNTEST_ASSERT_GT(AllRowValues.Num(), 0);
	UNTEST_EXPECT_EQ(AllRowValues.Num(), OriginalRows.Num());
	UNTEST_ASSERT_TRUE(AllRowValues[0].IsValid() && AllRowValues[0]->Type == EJson::Object);

	// --- Step 1: import a payload holding exactly ONE of the rows. Import
	// replaces (bPreserveExistingValues stays false), so the table must SHRINK to
	// that single row. A no-op import, an append-only import, or an import that
	// silently dropped everything all fail here.
	FString SingleRowJson;
	{
		TArray<TSharedPtr<FJsonValue>> OneRow;
		OneRow.Add(AllRowValues[0]);
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&SingleRowJson);
		UNTEST_ASSERT_TRUE(FJsonSerializer::Serialize(OneRow, Writer));
	}
	UNTEST_ASSERT_FALSE(SingleRowJson.IsEmpty());

	{
		const IClaireonTool::FToolResult R = ImportTest_ImportJson(ImportJsonDTPath, SingleRowJson);
		UNTEST_ASSERT_FALSE(R.bIsError);
		// Row count lives only in the prose for this tool family, so this is a
		// legitimate Summary assertion -- text shape, not a structured fact.
		UNTEST_EXPECT_TRUE(R.GetContentAsString().Contains(TEXT("Imported 1 rows.")));
	}

	const TSet<FName> AfterShrink = ImportTest_RowNames(ImportJsonDTPath);
	UNTEST_EXPECT_EQ(AfterShrink.Num(), 1);
	UNTEST_EXPECT_TRUE(OriginalRows.Includes(AfterShrink));

	// --- Step 2: import the full payload back. The table must GROW to the exact
	// original row set, which proves the importer parsed every row of a real
	// multi-column payload and keyed them correctly.
	{
		const IClaireonTool::FToolResult R = ImportTest_ImportJson(ImportJsonDTPath, JsonAll);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}

	const TSet<FName> AfterRestore = ImportTest_RowNames(ImportJsonDTPath);
	UNTEST_EXPECT_EQ(AfterRestore.Num(), OriginalRows.Num());
	UNTEST_EXPECT_TRUE(AfterRestore.Includes(OriginalRows));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, ImportCsvReplacesTableContents, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonDataTableImportSuccessTests;

	UNTEST_ASSERT_FALSE(SourceDTPath().IsEmpty());
	const bool bDuplicated =
		ClaireonDataTableTestsFixtures::EnsureFreshDuplicate(SourceDTPath(), ImportCsvDTPath);
	ON_SCOPE_EXIT { ClaireonDataTableTestsFixtures::DeleteDuplicate(ImportCsvDTPath); };
	if (!bDuplicated)
	{
		UE_LOG(LogTemp, Error, TEXT("[Claireon.DataTable] Could not duplicate fixture %s -> %s"),
			*SourceDTPath(), ImportCsvDTPath);
	}
	UNTEST_ASSERT_TRUE(bDuplicated);

	const TSet<FName> OriginalRows = ImportTest_RowNames(ImportCsvDTPath);
	UNTEST_ASSERT_GT(OriginalRows.Num(), 1);

	const FString CsvAll = ImportTest_ExportCsv(ImportCsvDTPath);
	UNTEST_ASSERT_FALSE(CsvAll.IsEmpty());

	// The header is line 0 of UDataTable::GetTableAsCSV and is always a single
	// line ("---" or ImportKeyField, then one cell per property). Data lines are
	// NOT safe to slice -- a multi-line FText cell wraps -- so the shrink payload
	// is built from the header alone plus one synthetic row of empty cells, which
	// keeps this test independent of the fixture's row struct.
	TArray<FString> CsvLines;
	CsvAll.ParseIntoArrayLines(CsvLines, /*bCullEmpty=*/true);
	UNTEST_ASSERT_GT(CsvLines.Num(), 1);

	const FString HeaderLine = CsvLines[0];
	int32 ValueColumnCount = 0;
	for (int32 CharIdx = 0; CharIdx < HeaderLine.Len(); ++CharIdx)
	{
		if (HeaderLine[CharIdx] == TEXT(','))
		{
			++ValueColumnCount;
		}
	}
	UNTEST_ASSERT_GT(ValueColumnCount, 0);

	static const TCHAR* SyntheticRowName = TEXT("_UNTEST_ImportCsvRow");
	FString MinimalCsv = HeaderLine + TEXT("\n") + SyntheticRowName;
	for (int32 Column = 0; Column < ValueColumnCount; ++Column)
	{
		MinimalCsv += TEXT(",");
	}
	MinimalCsv += TEXT("\n");

	// --- Step 1: replace the whole table with the single synthetic row.
	{
		const IClaireonTool::FToolResult R = ImportTest_ImportCsv(ImportCsvDTPath, MinimalCsv);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}

	const TSet<FName> AfterShrink = ImportTest_RowNames(ImportCsvDTPath);
	UNTEST_EXPECT_EQ(AfterShrink.Num(), 1);
	UNTEST_EXPECT_TRUE(AfterShrink.Contains(FName(SyntheticRowName)));

	// --- Step 2: import the full CSV export back. Growing a 1-row table to the
	// exact original row set is the real success-path assertion: the importer had
	// to parse every data line of a genuine multi-column export.
	{
		const IClaireonTool::FToolResult R = ImportTest_ImportCsv(ImportCsvDTPath, CsvAll);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}

	const TSet<FName> AfterRestore = ImportTest_RowNames(ImportCsvDTPath);
	UNTEST_EXPECT_EQ(AfterRestore.Num(), OriginalRows.Num());
	UNTEST_EXPECT_TRUE(AfterRestore.Includes(OriginalRows));
	UNTEST_EXPECT_FALSE(AfterRestore.Contains(FName(SyntheticRowName)));

	co_return;
}

// ============================================================================
// Composite table — Write rejection
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTable, CompositeRejectsMutation, UNTEST_TIMEOUTMS(10000))
{
	// Needs a real UCompositeDataTable; a host project may have none. Skip loudly --
	// a bare co_return makes a skipped test indistinguishable from a passing one.
	if (TestCompositeDTPath().IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Claireon.DataTable] CompositeRejectsMutation SKIPPED: project has no UCompositeDataTable"));
		co_return;
	}

	// Composite tables should report as composite in get_info
	{
		ClaireonTool_DataTableGetInfo InfoTool;
		TSharedPtr<FJsonObject> InfoArgs = MakeShared<FJsonObject>();
		InfoArgs->SetStringField(TEXT("asset_path"), TestCompositeDTPath());
		auto InfoResult = InfoTool.Execute(InfoArgs);
		UNTEST_ASSERT_FALSE(InfoResult.bIsError);
		UNTEST_ASSERT_PTR(InfoResult.Data.Get());
		bool bIsComposite = false;
		UNTEST_EXPECT_TRUE(InfoResult.Data->TryGetBoolField(TEXT("is_composite"), bIsComposite));
		UNTEST_EXPECT_TRUE(bIsComposite);
	}

	// add_row should fail on composite table
	{
		ClaireonTool_DataTableAddRow AddTool;
		TSharedPtr<FJsonObject> AddArgs = MakeShared<FJsonObject>();
		AddArgs->SetStringField(TEXT("asset_path"), TestCompositeDTPath());
		AddArgs->SetStringField(TEXT("row_name"), TEXT("_UNTEST_ShouldFail"));
		auto AddResult = AddTool.Execute(AddArgs);
		UNTEST_ASSERT_TRUE(AddResult.bIsError);
		UNTEST_EXPECT_TRUE(AddResult.GetContentAsString().Contains(TEXT("Composite")));
	}

	// remove_row should fail on composite table
	{
		ClaireonTool_DataTableRemoveRow RemoveTool;
		TSharedPtr<FJsonObject> RemoveArgs = MakeShared<FJsonObject>();
		RemoveArgs->SetStringField(TEXT("asset_path"), TestCompositeDTPath());
		RemoveArgs->SetStringField(TEXT("row_name"), TEXT("SomeRow"));
		auto RemoveResult = RemoveTool.Execute(RemoveArgs);
		UNTEST_ASSERT_TRUE(RemoveResult.bIsError);
		UNTEST_EXPECT_TRUE(RemoveResult.GetContentAsString().Contains(TEXT("Composite")));
	}

	// import_csv should fail on composite table
	{
		ClaireonTool_DataTableImportCsv ImportTool;
		TSharedPtr<FJsonObject> ImportArgs = MakeShared<FJsonObject>();
		ImportArgs->SetStringField(TEXT("asset_path"), TestCompositeDTPath());
		ImportArgs->SetStringField(TEXT("csv"), TEXT("---\nRowName,Col1\nRow1,Value1"));
		auto ImportResult = ImportTool.Execute(ImportArgs);
		UNTEST_ASSERT_TRUE(ImportResult.bIsError);
		UNTEST_EXPECT_TRUE(ImportResult.GetContentAsString().Contains(TEXT("Composite")));
	}

	co_return;
}

// ============================================================================
// Mutation lifecycle — Add, read back, duplicate, rename, remove
// ============================================================================

// Runs entirely on a COPY of the discovered source table. Every step here calls a
// tool that saves the package, so pointing it at the project's own table rewrote
// that .uasset on every run: the add/remove pair round-trips the rows, but the
// re-serialized package is still a diff, and it came back dirty in git while the
// test itself passed.
UNTEST_UNIT_OPTS(Claireon, DataTable, MutationAddAndRemoveLifecycle, UNTEST_TIMEOUTMS(60000))
{
	if (!ClaireonDataTableTestsFixtures::EnsureFreshDuplicate(SourceDTPath(), MutableDTPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.DataTable] Could not duplicate fixture %s -> %s; skipping."),
			*SourceDTPath(), MutableDTPath);
		co_return;
	}
	ON_SCOPE_EXIT { ClaireonDataTableTestsFixtures::DeleteDuplicate(MutableDTPath); };

	// --- Step 1: Add a temp row ---
	ClaireonTool_DataTableAddRow AddTool;
	{
		TSharedPtr<FJsonObject> AddArgs = MakeShared<FJsonObject>();
		AddArgs->SetStringField(TEXT("asset_path"), MutableDTPath);
		AddArgs->SetStringField(TEXT("row_name"), UntestTempRowA);
		auto AddResult = AddTool.Execute(AddArgs);
		UNTEST_ASSERT_FALSE(AddResult.bIsError);
		UNTEST_ASSERT_PTR(AddResult.Data.Get());
		bool bCreated = false;
		UNTEST_EXPECT_TRUE(AddResult.Data->TryGetBoolField(TEXT("created"), bCreated));
		UNTEST_EXPECT_TRUE(bCreated);
	}

	// --- Step 2: Verify row exists via get_row ---
	ClaireonTool_DataTableGetRowStructured GetTool;
	{
		TSharedPtr<FJsonObject> GetArgs = MakeShared<FJsonObject>();
		GetArgs->SetStringField(TEXT("asset_path"), MutableDTPath);
		GetArgs->SetStringField(TEXT("row_name"), UntestTempRowA);
		auto GetResult = GetTool.Execute(GetArgs);
		UNTEST_EXPECT_FALSE(GetResult.bIsError);
		UNTEST_EXPECT_TRUE(GetResult.GetContentAsString().Contains(UntestTempRowA));
	}

	// --- Step 3: Adding same row again should fail without allow_overwrite ---
	{
		TSharedPtr<FJsonObject> AddArgs = MakeShared<FJsonObject>();
		AddArgs->SetStringField(TEXT("asset_path"), MutableDTPath);
		AddArgs->SetStringField(TEXT("row_name"), UntestTempRowA);
		auto AddResult = AddTool.Execute(AddArgs);
		UNTEST_EXPECT_TRUE(AddResult.bIsError);
		UNTEST_EXPECT_TRUE(AddResult.GetContentAsString().Contains(TEXT("already exists")));
	}

	// --- Step 4: Duplicate the row ---
	ClaireonTool_DataTableDuplicateRow DupTool;
	{
		TSharedPtr<FJsonObject> DupArgs = MakeShared<FJsonObject>();
		DupArgs->SetStringField(TEXT("asset_path"), MutableDTPath);
		DupArgs->SetStringField(TEXT("source_row"), UntestTempRowA);
		DupArgs->SetStringField(TEXT("new_row_name"), UntestTempRowB);
		auto DupResult = DupTool.Execute(DupArgs);
		UNTEST_EXPECT_FALSE(DupResult.bIsError);
		UNTEST_EXPECT_TRUE(DupResult.GetContentAsString().Contains(TEXT("duplicated")));
	}

	// --- Step 5: Rename the duplicate ---
	ClaireonTool_DataTableRenameRow RenameTool;
	{
		TSharedPtr<FJsonObject> RenameArgs = MakeShared<FJsonObject>();
		RenameArgs->SetStringField(TEXT("asset_path"), MutableDTPath);
		RenameArgs->SetStringField(TEXT("row_name"), UntestTempRowB);
		RenameArgs->SetStringField(TEXT("new_name"), UntestTempRowC);
		auto RenameResult = RenameTool.Execute(RenameArgs);
		UNTEST_EXPECT_FALSE(RenameResult.bIsError);
		UNTEST_EXPECT_TRUE(RenameResult.GetContentAsString().Contains(TEXT("renamed")));
	}

	// --- Step 6: Move the original row down ---
	ClaireonTool_DataTableMoveRow MoveTool;
	{
		TSharedPtr<FJsonObject> MoveArgs = MakeShared<FJsonObject>();
		MoveArgs->SetStringField(TEXT("asset_path"), MutableDTPath);
		MoveArgs->SetStringField(TEXT("row_name"), UntestTempRowA);
		MoveArgs->SetStringField(TEXT("direction"), TEXT("down"));
		auto MoveResult = MoveTool.Execute(MoveArgs);
		// Move may fail if row is already at the bottom — use EXPECT
		UNTEST_EXPECT_FALSE(MoveResult.bIsError);
	}

	// --- Step 7: Cleanup — remove both rows ---
	ClaireonTool_DataTableRemoveRow RemoveTool;
	{
		TSharedPtr<FJsonObject> RemoveArgs = MakeShared<FJsonObject>();
		RemoveArgs->SetStringField(TEXT("asset_path"), MutableDTPath);
		RemoveArgs->SetStringField(TEXT("row_name"), UntestTempRowA);
		auto RemoveResult = RemoveTool.Execute(RemoveArgs);
		UNTEST_EXPECT_FALSE(RemoveResult.bIsError);
		if (!RemoveResult.bIsError && RemoveResult.Data.IsValid())
		{
			bool bRemoved = false;
			UNTEST_EXPECT_TRUE(RemoveResult.Data->TryGetBoolField(TEXT("removed"), bRemoved));
			UNTEST_EXPECT_TRUE(bRemoved);
		}
	}
	{
		TSharedPtr<FJsonObject> RemoveArgs = MakeShared<FJsonObject>();
		RemoveArgs->SetStringField(TEXT("asset_path"), MutableDTPath);
		RemoveArgs->SetStringField(TEXT("row_name"), UntestTempRowC);
		auto RemoveResult = RemoveTool.Execute(RemoveArgs);
		UNTEST_EXPECT_FALSE(RemoveResult.bIsError);
	}

	// Also try removing B in case rename failed and B still exists
	{
		TSharedPtr<FJsonObject> RemoveArgs = MakeShared<FJsonObject>();
		RemoveArgs->SetStringField(TEXT("asset_path"), MutableDTPath);
		RemoveArgs->SetStringField(TEXT("row_name"), UntestTempRowB);
		RemoveTool.Execute(RemoveArgs); // Don't assert — may not exist
	}

	// --- Step 8: Verify row no longer exists ---
	{
		TSharedPtr<FJsonObject> GetArgs = MakeShared<FJsonObject>();
		GetArgs->SetStringField(TEXT("asset_path"), MutableDTPath);
		GetArgs->SetStringField(TEXT("row_name"), UntestTempRowA);
		auto GetResult = GetTool.Execute(GetArgs);
		UNTEST_EXPECT_TRUE(GetResult.bIsError);
		UNTEST_EXPECT_TRUE(GetResult.GetContentAsString().Contains(TEXT("not found")));
	}

	co_return;
}


// ============================================================================
// Functional tests -- datatable_get_row (structured)
// ============================================================================

namespace ClaireonDataTableTests_Private
{
	// Helper: pick the first row name from a DataTable (loaded via the helper).
	// Returns empty FString if the table is missing or empty.
	static FString PickFirstRowName_DataTableStructuredTests(const FString& AssetPath)
	{
		FString LoadError;
		UDataTable* DT = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, LoadError);
		if (!IsValid(DT)) { return FString(); }
		const TMap<FName, uint8*>& RowMap = DT->GetRowMap();
		for (const auto& Pair : RowMap)
		{
			return Pair.Key.ToString();
		}
		return FString();
	}

	// Helper: check whether the friendly-keyed JSON object has any key containing
	// the BP user-defined-struct GUID-suffix shape (_<digits>_<32 hex>).
	static bool ContainsGuidSuffixedKey_DataTableStructuredTests(const TSharedPtr<FJsonObject>& Obj)
	{
		if (!Obj.IsValid()) { return false; }
		for (const auto& Pair : Obj->Values)
		{
			const FString& Key = Pair.Key;
			// Look for an underscore followed by digits, then another underscore, then >= 16 hex digits.
			// The full suffix is 32 chars, but checking for a leading run is sufficient as a signal.
			int32 Pos = 0;
			while (Pos < Key.Len())
			{
				int32 UnderscoreIdx = Key.Find(TEXT("_"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos);
				if (UnderscoreIdx == INDEX_NONE) { break; }
				// digits after underscore
				int32 i = UnderscoreIdx + 1;
				while (i < Key.Len() && FChar::IsDigit(Key[i])) { ++i; }
				if (i > UnderscoreIdx + 1 && i < Key.Len() && Key[i] == TEXT('_'))
				{
					// hex run after second underscore
					int32 HexStart = i + 1;
					int32 HexLen = 0;
					while (HexStart + HexLen < Key.Len() && FChar::IsHexDigit(Key[HexStart + HexLen])) { ++HexLen; }
					if (HexLen >= 16)
					{
						return true;
					}
				}
				Pos = UnderscoreIdx + 1;
			}
		}
		return false;
	}

	static FString SerializeJsonObjectToString_DataTableStructuredTests(const TSharedPtr<FJsonObject>& Obj)
	{
		FString Out;
		if (!Obj.IsValid()) { return Out; }
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
		return Out;
	}
}
using namespace ClaireonDataTableTests_Private;

UNTEST_UNIT_OPTS(Claireon, DataTable, GetRowStructuredPrimitive, UNTEST_TIMEOUTMS(15000))
{
	// Primitive row test against the discovered source table.
	const FString FirstRow = PickFirstRowName_DataTableStructuredTests(SourceDTPath());
	UNTEST_ASSERT_TRUE(!FirstRow.IsEmpty());

	ClaireonTool_DataTableGetRowStructured Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), SourceDTPath());
	Args->SetStringField(TEXT("row_name"), FirstRow);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Top-level shape
	FString TablePath;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("table_path"), TablePath));
	UNTEST_EXPECT_TRUE(TablePath.Contains(SourceDTName()));

	FString RowName;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("row_name"), RowName));
	UNTEST_EXPECT_STREQ(RowName, FirstRow);

	const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("values"), ValuesObj));
	UNTEST_ASSERT_TRUE(ValuesObj && (*ValuesObj).IsValid());
	UNTEST_EXPECT_TRUE((*ValuesObj)->Values.Num() > 0);

	// No GUID-suffixed keys at the row level for native struct rows.
	UNTEST_EXPECT_FALSE(ContainsGuidSuffixedKey_DataTableStructuredTests(*ValuesObj));

	// Scalar properties of the row struct should land as native JSON types where applicable.
	// Walk the struct so we know what types to assert without hardcoding column names.
	FString LoadError;
	UDataTable* DT = ClaireonDataTableHelpers::LoadDataTableAsset(SourceDTPath(), LoadError);
	UNTEST_ASSERT_PTR(DT);
	const UScriptStruct* RowStruct = DT->GetRowStruct();
	UNTEST_ASSERT_PTR(RowStruct);

	int32 ScalarMatchCount = 0;
	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		const FProperty* Prop = *It;
		const FString FriendlyName = ClaireonStructReflection::GetFriendlyPropertyName(Prop);
		const TSharedPtr<FJsonValue>* FieldValPtr = (*ValuesObj)->Values.Find(FriendlyName);
		if (!FieldValPtr || !FieldValPtr->IsValid()) { continue; }
		const TSharedPtr<FJsonValue>& FieldVal = *FieldValPtr;

		if (CastField<FBoolProperty>(Prop))
		{
			UNTEST_EXPECT_TRUE(FieldVal->Type == EJson::Boolean);
			++ScalarMatchCount;
		}
		else if (CastField<FIntProperty>(Prop) || CastField<FInt64Property>(Prop) || CastField<FFloatProperty>(Prop) || CastField<FDoubleProperty>(Prop))
		{
			UNTEST_EXPECT_TRUE(FieldVal->Type == EJson::Number);
			++ScalarMatchCount;
		}
		else if (CastField<FStrProperty>(Prop) || CastField<FNameProperty>(Prop))
		{
			UNTEST_EXPECT_TRUE(FieldVal->Type == EJson::String);
			++ScalarMatchCount;
		}
		else if (CastField<FEnumProperty>(Prop))
		{
			// Enum -> { value, name }
			UNTEST_ASSERT_TRUE(FieldVal->Type == EJson::Object);
			TSharedPtr<FJsonObject> EnumObj = FieldVal->AsObject();
			UNTEST_ASSERT_PTR(EnumObj.Get());
			UNTEST_EXPECT_TRUE(EnumObj->HasField(TEXT("value")));
			UNTEST_EXPECT_TRUE(EnumObj->HasField(TEXT("name")));
			++ScalarMatchCount;
		}
	}
	UNTEST_EXPECT_TRUE(ScalarMatchCount > 0);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, GetRowStructuredMapColumn, UNTEST_TIMEOUTMS(30000))
{
	// TMap-column functional test. Skip cleanly if the project has no such table.
	// Logged as a Warning: a bare co_return with only a comment makes a silently
	// skipped test indistinguishable from a passing one in the run output.
	const FString& MapDTPath = TestMapColumnDTPath();
	if (MapDTPath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Claireon.DataTable] GetRowStructuredMapColumn SKIPPED: project has no data table with a TMap column"));
		co_return;
	}

	FString LoadError;
	UDataTable* MapDT = ClaireonDataTableHelpers::LoadDataTableAsset(MapDTPath, LoadError);
	if (!IsValid(MapDT))
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.DataTable] GetRowStructuredMapColumn SKIPPED: fixture %s not loadable (%s)"),
			*MapDTPath, *LoadError);
		co_return;
	}

	const FString FirstRow = PickFirstRowName_DataTableStructuredTests(MapDTPath);
	if (FirstRow.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Claireon.DataTable] GetRowStructuredMapColumn SKIPPED: fixture %s has no rows"),
			*MapDTPath);
		co_return;
	}

	ClaireonTool_DataTableGetRowStructured Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), MapDTPath);
	Args->SetStringField(TEXT("row_name"), FirstRow);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("values"), ValuesObj));
	UNTEST_ASSERT_TRUE(ValuesObj && (*ValuesObj).IsValid());

	// BP user-defined-struct property names should not carry their GUID suffix.
	UNTEST_EXPECT_FALSE(ContainsGuidSuffixedKey_DataTableStructuredTests(*ValuesObj));

	// Look for at least one map field (TMap surfaces as a JSON array of { key, value }).
	bool bFoundMapArray = false;
	for (const auto& Pair : (*ValuesObj)->Values)
	{
		const TSharedPtr<FJsonValue>& V = Pair.Value;
		if (V.IsValid() && V->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Arr = V->AsArray();
			if (Arr.Num() > 0 && Arr[0].IsValid() && Arr[0]->Type == EJson::Object)
			{
				TSharedPtr<FJsonObject> Entry = Arr[0]->AsObject();
				if (Entry.IsValid() && Entry->HasField(TEXT("key")) && Entry->HasField(TEXT("value")))
				{
					bFoundMapArray = true;
					break;
				}
			}
		}
	}
	// Discovery guarantees the row STRUCT has a TMap column, but not that this row's
	// map is populated -- an empty TMap legitimately serializes to an empty array, so
	// this is a skip rather than a failure.
	if (!bFoundMapArray)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Claireon.DataTable] GetRowStructuredMapColumn: row '%s' of %s has no populated map column; "
				 "map-emission shape not exercised"),
			*FirstRow, *MapDTPath);
	}

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, GetRowStructuredIncludeSchema, UNTEST_TIMEOUTMS(30000))
{
	// include_schema=true round-trip.
	//
	// The schema-equality assertion below only runs for STRUCT-typed columns, so the
	// fixture must have at least one. Picking a table that merely loads is not enough:
	// a row struct with no FStructProperty members never enters the loop body --
	// harmless while the floor assertion was the tautology
	// UNTEST_EXPECT_TRUE(StructColumnsChecked >= 0), fatal once it became
	// EXPECT_GT(.., 0). Discovery therefore selects on the row struct, not on
	// loadability.
	const FString& AssetPath = TestStructColumnDTPath();
	if (AssetPath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[Claireon.DataTable] GetRowStructuredIncludeSchema SKIPPED: project has no data table with a struct column"));
		co_return;
	}

	FString LoadError;
	UDataTable* DT = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, LoadError);
	UNTEST_ASSERT_PTR(DT);

	const FString FirstRow = PickFirstRowName_DataTableStructuredTests(AssetPath);
	UNTEST_ASSERT_TRUE(!FirstRow.IsEmpty());

	ClaireonTool_DataTableGetRowStructured Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("row_name"), FirstRow);
	Args->SetBoolField(TEXT("include_schema"), true);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Top-level keys exactly: table_path, row_name, values, schema.
	UNTEST_EXPECT_TRUE(Result.Data->HasField(TEXT("table_path")));
	UNTEST_EXPECT_TRUE(Result.Data->HasField(TEXT("row_name")));
	UNTEST_EXPECT_TRUE(Result.Data->HasField(TEXT("values")));
	UNTEST_EXPECT_TRUE(Result.Data->HasField(TEXT("schema")));
	UNTEST_EXPECT_TRUE(Result.Data->Values.Num() == 4);

	const TSharedPtr<FJsonObject>* ValuesObj = nullptr;
	const TSharedPtr<FJsonObject>* SchemaObj = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("values"), ValuesObj));
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("schema"), SchemaObj));
	UNTEST_ASSERT_TRUE(ValuesObj && (*ValuesObj).IsValid());
	UNTEST_ASSERT_TRUE(SchemaObj && (*SchemaObj).IsValid());

	// schema keys == values keys.
	TArray<FString> ValueKeys;
	(*ValuesObj)->Values.GenerateKeyArray(ValueKeys);
	TArray<FString> SchemaKeys;
	(*SchemaObj)->Values.GenerateKeyArray(SchemaKeys);
	ValueKeys.Sort();
	SchemaKeys.Sort();
	UNTEST_EXPECT_TRUE(ValueKeys == SchemaKeys);

	// For at least one struct-typed column, schema[col] string-equals SerializeStructSchema for that column's struct.
	UScriptStruct* RowStruct = const_cast<UScriptStruct*>(DT->GetRowStruct());
	UNTEST_ASSERT_PTR(RowStruct);
	// Non-empty floor. Root cause: the schema-equality assertion lives inside this
	// iterator, so if the row struct had no struct columns (or their schema keys were
	// missing) the loop body would never run -- and the old
	// UNTEST_EXPECT_TRUE(StructColumnsChecked >= 0) is a literal tautology, so the
	// test passed having compared nothing. The fixture was discovered BY having struct
	// columns, so requiring at least one comparison is safe and makes the assertion
	// reachable.
	int32 StructPropsInRowStruct = 0;
	int32 StructColumnsChecked = 0;
	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		const FProperty* Prop = *It;
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
			++StructPropsInRowStruct;
			const FString Friendly = ClaireonStructReflection::GetFriendlyPropertyName(Prop);
			const TSharedPtr<FJsonValue>* SchemaValPtr = (*SchemaObj)->Values.Find(Friendly);
			if (!SchemaValPtr || !SchemaValPtr->IsValid()) { continue; }
			TSharedPtr<FJsonObject> SchemaFieldObj = (*SchemaValPtr)->AsObject();
			if (!SchemaFieldObj.IsValid()) { continue; }

			TSharedPtr<FJsonObject> Expected = ClaireonStructReflection::SerializeStructSchema(StructProp->Struct, /*bIncludeDefaults=*/false, /*bIncludeMetadata=*/false);
			const FString GotStr = SerializeJsonObjectToString_DataTableStructuredTests(SchemaFieldObj);
			const FString WantStr = SerializeJsonObjectToString_DataTableStructuredTests(Expected);
			UNTEST_EXPECT_STREQ(GotStr, WantStr);
			++StructColumnsChecked;
		}
	}
	UNTEST_EXPECT_GT(StructPropsInRowStruct, 0);
	UNTEST_EXPECT_GT(StructColumnsChecked, 0);

	co_return;
}

// Recursion-cap test: best-effort. We do not ship a self-referential
// fixture, so we document the cap behavior here (MaxDepth=32, sentinel
// shape) without runtime coverage. The cap is exercised manually via the
// tool description.

// Friendly-name collision test: same situation -- behavior is documented but no convenient
// fixture exists in the test project, so it is not exercised at runtime.

// ============================================================================
// Composite refresh — rebuild fixes a stale cache
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTable, CompositeRefreshFixesStaleCache, UNTEST_TIMEOUTMS(30000))
{
	// Sandbox package on disk so RefreshCompositeDataTable's save can succeed.
	const FString TimeStamp = FDateTime::UtcNow().ToString(TEXT("%Y%m%d_%H%M%S_%f"));
	const FString PkgPath = FString::Printf(TEXT("/Game/__ClaireonDataTableTests/CompositeRefresh_%s"), *TimeStamp);
	UPackage* Pkg = CreatePackage(*PkgPath);
	UNTEST_ASSERT_PTR(Pkg);

	UScriptStruct* RowStruct = FTableRowBase::StaticStruct();
	UNTEST_ASSERT_PTR(RowStruct);

	// Parent table with rows A and B.
	UDataTable* Parent = NewObject<UDataTable>(Pkg, TEXT("DT_RefreshParent"), RF_Public | RF_Standalone);
	UNTEST_ASSERT_PTR(Parent);
	Parent->RowStruct = RowStruct;
	{
		FTableRowBase RowA;
		FTableRowBase RowB;
		Parent->AddRow(TEXT("A"), RowA);
		Parent->AddRow(TEXT("B"), RowB);
	}
	UNTEST_ASSERT_TRUE(Parent->GetRowMap().Contains(TEXT("A")));
	UNTEST_ASSERT_TRUE(Parent->GetRowMap().Contains(TEXT("B")));

	// Composite over the parent. AppendParentTables builds the cached RowMap.
	UCompositeDataTable* Composite = NewObject<UCompositeDataTable>(Pkg, TEXT("DT_RefreshComposite"), RF_Public | RF_Standalone);
	UNTEST_ASSERT_PTR(Composite);
	Composite->RowStruct = RowStruct;
	Composite->AppendParentTables(TArray<UDataTable*>{ Parent });
	UNTEST_ASSERT_TRUE(Composite->GetRowMap().Contains(TEXT("A")));
	UNTEST_ASSERT_TRUE(Composite->GetRowMap().Contains(TEXT("B")));

	// Make the cache unambiguously stale: clear the inherited RowMap WITHOUT going
	// through the composite override (which would also wipe ParentTables). The base
	// EmptyTable clears only the cached rows, leaving parent linkage intact.
	Composite->UDataTable::EmptyTable();
	UNTEST_ASSERT_FALSE(Composite->GetRowMap().Contains(TEXT("A")));
	UNTEST_ASSERT_FALSE(Composite->GetRowMap().Contains(TEXT("B")));

	// Refresh must rebuild the cache from the (still-attached) parent and save.
	FString RefreshErr;
	const bool bRefreshed = ClaireonDataTableHelpers::RefreshCompositeDataTable(Composite, RefreshErr);
	UNTEST_EXPECT_TRUE(bRefreshed);
	if (!bRefreshed)
	{
		UNTEST_EXPECT_TRUE(RefreshErr.IsEmpty());
	}
	UNTEST_EXPECT_TRUE(Composite->GetRowMap().Contains(TEXT("A")));
	UNTEST_EXPECT_TRUE(Composite->GetRowMap().Contains(TEXT("B")));

	// Null guard.
	{
		FString NullErr;
		UNTEST_EXPECT_FALSE(ClaireonDataTableHelpers::RefreshCompositeDataTable(nullptr, NullErr));
		UNTEST_EXPECT_FALSE(NullErr.IsEmpty());
	}

	// Cleanup both sandbox assets; leaving either behind strands the package file in Content/.
	ClaireonTestAssetDeletion::DeleteAssetForTest(FString::Printf(TEXT("%s.DT_RefreshComposite"), *PkgPath));
	ClaireonTestAssetDeletion::DeleteAssetForTest(FString::Printf(TEXT("%s.DT_RefreshParent"), *PkgPath));

	co_return;
}


#endif // WITH_UNTESTED
