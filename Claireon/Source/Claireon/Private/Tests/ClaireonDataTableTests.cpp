// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonTool_DataTableSearch.h"
#include "Tools/ClaireonTool_DataTableGetInfo.h"
#include "Tools/ClaireonTool_DataTableGetRows.h"
#include "Tools/ClaireonTool_DataTableGetRow.h"
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
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Test asset paths
// ---------------------------------------------------------------------------
static const TCHAR* TestDTPath = TEXT("/Game/Subsystems/Progression/DT_Maps");
static const TCHAR* TestCompositeDTPath = TEXT("/Game/Subsystems/Progression/DT_Composite_Items");
static const TCHAR* TestBadDTPath = TEXT("/Game/DoesNotExist/DT_Fake");

// Unique prefix for rows created by mutation tests — prevents collisions
static const TCHAR* UntestTempRowA = TEXT("_UNTEST_TempRow_A");
static const TCHAR* UntestTempRowB = TEXT("_UNTEST_TempRow_B");
static const TCHAR* UntestTempRowC = TEXT("_UNTEST_TempRow_C");

// ============================================================================
// Schema validation — Read tools
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTableSchema, ReadToolsValid, UNTEST_TIMEOUTMS(1000))
{
	// claireon.datatable_search
	{
		ClaireonTool_DataTableSearch Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_search"));
		UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		FString Type;
		UNTEST_EXPECT_TRUE(Schema->TryGetStringField(TEXT("type"), Type));
		UNTEST_EXPECT_STREQ(Type, TEXT("object"));
		const TSharedPtr<FJsonObject>* Props;
		UNTEST_EXPECT_TRUE(Schema->TryGetObjectField(TEXT("properties"), Props));
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 1);
	}

	// get_datatable_info
	{
		ClaireonTool_DataTableGetInfo Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_get_info"));
		UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 1);
	}

	// get_datatable_rows
	{
		ClaireonTool_DataTableGetRows Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_get_rows"));
		UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
	}

	// get_datatable_row
	{
		ClaireonTool_DataTableGetRow Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_get_row"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 2);
	}

	// find_datatable_rows
	{
		ClaireonTool_DataTableFindRows Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_find_rows"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 3);
	}

	co_return;
}

// ============================================================================
// Schema validation — Mutation tools
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTableSchema, MutationToolsValid, UNTEST_TIMEOUTMS(1000))
{
	// add_datatable_row
	{
		ClaireonTool_DataTableAddRow Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_add_row"));
		UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 2);
	}

	// remove_datatable_row
	{
		ClaireonTool_DataTableRemoveRow Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_remove_row"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
	}

	// claireon.datatable_duplicate_row
	{
		ClaireonTool_DataTableDuplicateRow Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_duplicate_row"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 3);
	}

	// claireon.datatable_rename_row
	{
		ClaireonTool_DataTableRenameRow Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_rename_row"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 3);
	}

	// claireon.datatable_move_row
	{
		ClaireonTool_DataTableMoveRow Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_move_row"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 3);
	}

	// set_datatable_row
	{
		ClaireonTool_DataTableSetRowValues Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_set_row_values"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 3);
	}

	co_return;
}

// ============================================================================
// Schema validation — Import/Export tools
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTableSchema, ImportExportToolsValid, UNTEST_TIMEOUTMS(1000))
{
	// claireon.datatable_export_json
	{
		ClaireonTool_DataTableExportJson Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_export_json"));
		UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
	}

	// claireon.datatable_import_json
	{
		ClaireonTool_DataTableImportJson Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_import_json"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 2);
	}

	// claireon.datatable_export_csv
	{
		ClaireonTool_DataTableExportCsv Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_export_csv"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
	}

	// claireon.datatable_import_csv
	{
		ClaireonTool_DataTableImportCsv Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("claireon.datatable_import_csv"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
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
	ClaireonTool_DataTableGetRow Tool;

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
		Args->SetStringField(TEXT("asset_path"), TestDTPath);
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
		Args->SetStringField(TEXT("asset_path"), TestDTPath);
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
	Args->SetStringField(TEXT("asset_path"), TestDTPath);
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
	Args->SetStringField(TEXT("asset_path"), TestDTPath);
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
		Args->SetStringField(TEXT("asset_path"), TestDTPath);
		Args->SetStringField(TEXT("row_name"), TEXT("SomeRow"));
		auto Result = Tool.Execute(Args);
		UNTEST_ASSERT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("direction")));
	}

	// Invalid direction value
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), TestDTPath);
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
	Args->SetStringField(TEXT("asset_path"), TestDTPath);
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
	Args->SetStringField(TEXT("asset_path"), TestDTPath);
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
	ClaireonTool_DataTableGetRow Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestDTPath);
	Args->SetStringField(TEXT("row_name"), TEXT("_UNTEST_NonexistentRow_999"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("not found")));
	co_return;
}

// ============================================================================
// Read operations — Valid asset
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTable, SearchFindsKnownTable, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_DataTableSearch Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("query"), TEXT("DT_Maps"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("DT_Maps")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("Found")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, GetInfoReturnsMetadata, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_DataTableGetInfo Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestDTPath);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Structured data fields
	FString TablePath;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("table_path"), TablePath));
	UNTEST_EXPECT_TRUE(TablePath.Contains(TEXT("DT_Maps")));

	FString RowStruct;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("row_struct"), RowStruct));
	UNTEST_EXPECT_TRUE(!RowStruct.IsEmpty());

	double RowCount = 0.0;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetNumberField(TEXT("row_count"), RowCount));
	UNTEST_EXPECT_TRUE(RowCount >= 0.0);

	bool bIsComposite = false;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetBoolField(TEXT("is_composite"), bIsComposite));
	UNTEST_EXPECT_FALSE(bIsComposite);

	const TArray<TSharedPtr<FJsonValue>>* Columns;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetArrayField(TEXT("columns"), Columns));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, GetRowsReturnsList, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_DataTableGetRows Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestDTPath);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Structured data fields
	double TotalRows = 0.0;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetNumberField(TEXT("total_rows"), TotalRows));

	double ReturnedRows = 0.0;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetNumberField(TEXT("returned_rows"), ReturnedRows));
	UNTEST_EXPECT_TRUE(ReturnedRows >= 0.0);

	const TArray<TSharedPtr<FJsonValue>>* RowsArray;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetArrayField(TEXT("rows"), RowsArray));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, GetRowsPagination, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_DataTableGetRows Tool;

	// Request only 2 rows with offset
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestDTPath);
	Args->SetNumberField(TEXT("max_rows"), 2);
	Args->SetNumberField(TEXT("offset"), 0);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Summary contains pagination info like "DT_Maps: showing rows 1-2 of N"
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

UNTEST_UNIT_OPTS(Claireon, DataTable, ExportJsonValid, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_DataTableExportJson Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestDTPath);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	FString Output = Result.GetContentAsString();
	// JSON output should contain array brackets
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("[")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("]")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, ExportCsvValid, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_DataTableExportCsv Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestDTPath);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	FString Output = Result.GetContentAsString();
	// CSV should have a header row with commas
	UNTEST_EXPECT_TRUE(Output.Len() > 10);

	co_return;
}

// ============================================================================
// Composite table — Write rejection
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTable, CompositeRejectsMutation, UNTEST_TIMEOUTMS(10000))
{
	// Composite tables should report as composite in get_info
	{
		ClaireonTool_DataTableGetInfo InfoTool;
		TSharedPtr<FJsonObject> InfoArgs = MakeShared<FJsonObject>();
		InfoArgs->SetStringField(TEXT("asset_path"), TestCompositeDTPath);
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
		AddArgs->SetStringField(TEXT("asset_path"), TestCompositeDTPath);
		AddArgs->SetStringField(TEXT("row_name"), TEXT("_UNTEST_ShouldFail"));
		auto AddResult = AddTool.Execute(AddArgs);
		UNTEST_ASSERT_TRUE(AddResult.bIsError);
		UNTEST_EXPECT_TRUE(AddResult.GetContentAsString().Contains(TEXT("Composite")));
	}

	// remove_row should fail on composite table
	{
		ClaireonTool_DataTableRemoveRow RemoveTool;
		TSharedPtr<FJsonObject> RemoveArgs = MakeShared<FJsonObject>();
		RemoveArgs->SetStringField(TEXT("asset_path"), TestCompositeDTPath);
		RemoveArgs->SetStringField(TEXT("row_name"), TEXT("SomeRow"));
		auto RemoveResult = RemoveTool.Execute(RemoveArgs);
		UNTEST_ASSERT_TRUE(RemoveResult.bIsError);
		UNTEST_EXPECT_TRUE(RemoveResult.GetContentAsString().Contains(TEXT("Composite")));
	}

	// import_csv should fail on composite table
	{
		ClaireonTool_DataTableImportCsv ImportTool;
		TSharedPtr<FJsonObject> ImportArgs = MakeShared<FJsonObject>();
		ImportArgs->SetStringField(TEXT("asset_path"), TestCompositeDTPath);
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

UNTEST_UNIT_OPTS(Claireon, DataTable, MutationAddAndRemoveLifecycle, UNTEST_TIMEOUTMS(30000))
{
	// --- Step 1: Add a temp row ---
	ClaireonTool_DataTableAddRow AddTool;
	{
		TSharedPtr<FJsonObject> AddArgs = MakeShared<FJsonObject>();
		AddArgs->SetStringField(TEXT("asset_path"), TestDTPath);
		AddArgs->SetStringField(TEXT("row_name"), UntestTempRowA);
		auto AddResult = AddTool.Execute(AddArgs);
		UNTEST_ASSERT_FALSE(AddResult.bIsError);
		UNTEST_ASSERT_PTR(AddResult.Data.Get());
		bool bCreated = false;
		UNTEST_EXPECT_TRUE(AddResult.Data->TryGetBoolField(TEXT("created"), bCreated));
		UNTEST_EXPECT_TRUE(bCreated);
	}

	// --- Step 2: Verify row exists via get_row ---
	ClaireonTool_DataTableGetRow GetTool;
	{
		TSharedPtr<FJsonObject> GetArgs = MakeShared<FJsonObject>();
		GetArgs->SetStringField(TEXT("asset_path"), TestDTPath);
		GetArgs->SetStringField(TEXT("row_name"), UntestTempRowA);
		auto GetResult = GetTool.Execute(GetArgs);
		UNTEST_EXPECT_FALSE(GetResult.bIsError);
		UNTEST_EXPECT_TRUE(GetResult.GetContentAsString().Contains(UntestTempRowA));
	}

	// --- Step 3: Adding same row again should fail without allow_overwrite ---
	{
		TSharedPtr<FJsonObject> AddArgs = MakeShared<FJsonObject>();
		AddArgs->SetStringField(TEXT("asset_path"), TestDTPath);
		AddArgs->SetStringField(TEXT("row_name"), UntestTempRowA);
		auto AddResult = AddTool.Execute(AddArgs);
		UNTEST_EXPECT_TRUE(AddResult.bIsError);
		UNTEST_EXPECT_TRUE(AddResult.GetContentAsString().Contains(TEXT("already exists")));
	}

	// --- Step 4: Duplicate the row ---
	ClaireonTool_DataTableDuplicateRow DupTool;
	{
		TSharedPtr<FJsonObject> DupArgs = MakeShared<FJsonObject>();
		DupArgs->SetStringField(TEXT("asset_path"), TestDTPath);
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
		RenameArgs->SetStringField(TEXT("asset_path"), TestDTPath);
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
		MoveArgs->SetStringField(TEXT("asset_path"), TestDTPath);
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
		RemoveArgs->SetStringField(TEXT("asset_path"), TestDTPath);
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
		RemoveArgs->SetStringField(TEXT("asset_path"), TestDTPath);
		RemoveArgs->SetStringField(TEXT("row_name"), UntestTempRowC);
		auto RemoveResult = RemoveTool.Execute(RemoveArgs);
		UNTEST_EXPECT_FALSE(RemoveResult.bIsError);
	}

	// Also try removing B in case rename failed and B still exists
	{
		TSharedPtr<FJsonObject> RemoveArgs = MakeShared<FJsonObject>();
		RemoveArgs->SetStringField(TEXT("asset_path"), TestDTPath);
		RemoveArgs->SetStringField(TEXT("row_name"), UntestTempRowB);
		RemoveTool.Execute(RemoveArgs); // Don't assert — may not exist
	}

	// --- Step 8: Verify row no longer exists ---
	{
		TSharedPtr<FJsonObject> GetArgs = MakeShared<FJsonObject>();
		GetArgs->SetStringField(TEXT("asset_path"), TestDTPath);
		GetArgs->SetStringField(TEXT("row_name"), UntestTempRowA);
		auto GetResult = GetTool.Execute(GetArgs);
		UNTEST_EXPECT_TRUE(GetResult.bIsError);
		UNTEST_EXPECT_TRUE(GetResult.GetContentAsString().Contains(TEXT("not found")));
	}

	co_return;
}


#endif // WITH_UNTESTED
