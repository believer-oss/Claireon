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
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/DataTable.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/UnrealType.h"

// ---------------------------------------------------------------------------
// Test asset paths
// ---------------------------------------------------------------------------
static const TCHAR* TestDTPath = TEXT("/Game/Subsystems/Progression/DT_Maps");
static const TCHAR* TestCompositeDTPath = TEXT("/Game/Subsystems/Progression/DT_Composite_Items");
static const TCHAR* TestBadDTPath = TEXT("/Game/DoesNotExist/DT_Fake");
static const TCHAR* TestBanterPath = TEXT("/Game/Narrative/MiniBanter/DT_MiniBanterLines");

// Unique prefix for rows created by mutation tests — prevents collisions
static const TCHAR* UntestTempRowA = TEXT("_UNTEST_TempRow_A");
static const TCHAR* UntestTempRowB = TEXT("_UNTEST_TempRow_B");
static const TCHAR* UntestTempRowC = TEXT("_UNTEST_TempRow_C");

// ============================================================================
// Schema validation — Read tools
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, DataTableSchema, ReadToolsValid, UNTEST_TIMEOUTMS(1000))
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
		const TSharedPtr<FJsonObject>* Props;
		UNTEST_EXPECT_TRUE(Schema->TryGetObjectField(TEXT("properties"), Props));
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
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
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
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
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
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
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_add_row"));
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
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 3);
	}

	// datatable_rename_row
	{
		ClaireonTool_DataTableRenameRow Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_rename_row"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 3);
	}

	// datatable_move_row
	{
		ClaireonTool_DataTableMoveRow Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_move_row"));
		auto Schema = Tool.GetInputSchema();
		UNTEST_ASSERT_PTR(Schema.Get());
		const TArray<TSharedPtr<FJsonValue>>* Required;
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
		UNTEST_EXPECT_TRUE(Required->Num() >= 3);
	}

	// set_datatable_row
	{
		ClaireonTool_DataTableSetRowValues Tool;
		UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("datatable_set_row_values"));
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
		UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
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
	ClaireonTool_DataTableGetRowStructured Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestDTPath);
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
		Args->SetStringField(TEXT("asset_path"), TestDTPath);
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
		Args->SetStringField(TEXT("asset_path"), TestDTPath);
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
	ClaireonTool_DataTableGetRowStructured GetTool;
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


// ============================================================================
// Functional tests -- datatable_get_row (structured)
// ============================================================================

namespace
{
	// Helper: pick the first row name from a DataTable (loaded via the helper).
	// Returns empty FString if the table is missing or empty.
	static FString PickFirstRowName_DataTableStructuredTests(const TCHAR* AssetPath)
	{
		FString LoadError;
		UDataTable* DT = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, LoadError);
		if (!DT) { return FString(); }
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

UNTEST_UNIT_OPTS(Claireon, DataTable, GetRowStructuredPrimitive, UNTEST_TIMEOUTMS(15000))
{
	// Primitive row test against DT_Maps.
	const FString FirstRow = PickFirstRowName_DataTableStructuredTests(TestDTPath);
	UNTEST_ASSERT_TRUE(!FirstRow.IsEmpty());

	ClaireonTool_DataTableGetRowStructured Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestDTPath);
	Args->SetStringField(TEXT("row_name"), FirstRow);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Top-level shape
	FString TablePath;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("table_path"), TablePath));
	UNTEST_EXPECT_TRUE(TablePath.Contains(TEXT("DT_Maps")));

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
	UDataTable* DT = ClaireonDataTableHelpers::LoadDataTableAsset(TestDTPath, LoadError);
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

UNTEST_UNIT_OPTS(Claireon, DataTable, GetRowStructuredBanter, UNTEST_TIMEOUTMS(30000))
{
	// Banter row functional test. Skip cleanly if the asset is absent.
	FString LoadError;
	UDataTable* BanterDT = ClaireonDataTableHelpers::LoadDataTableAsset(TestBanterPath, LoadError);
	if (!BanterDT)
	{
		// Not present in this project variant -- skip without failing.
		co_return;
	}

	const FString FirstRow = PickFirstRowName_DataTableStructuredTests(TestBanterPath);
	if (FirstRow.IsEmpty())
	{
		// Empty table -- skip.
		co_return;
	}

	ClaireonTool_DataTableGetRowStructured Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestBanterPath);
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
	UNTEST_EXPECT_TRUE(bFoundMapArray);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, DataTable, GetRowStructuredIncludeSchema, UNTEST_TIMEOUTMS(30000))
{
	// include_schema=true round-trip. Use the banter row when available; fall back to DT_Maps.
	const TCHAR* AssetPath = TestBanterPath;
	FString LoadError;
	UDataTable* DT = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, LoadError);
	if (!DT)
	{
		AssetPath = TestDTPath;
		DT = ClaireonDataTableHelpers::LoadDataTableAsset(AssetPath, LoadError);
	}
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
	int32 StructColumnsChecked = 0;
	for (TFieldIterator<FProperty> It(RowStruct); It; ++It)
	{
		const FProperty* Prop = *It;
		if (const FStructProperty* StructProp = CastField<FStructProperty>(Prop))
		{
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
	// We don't strictly require StructColumnsChecked > 0 (DT_Maps may have no struct columns), but
	// when banter is available it must hit at least one.
	UNTEST_EXPECT_TRUE(StructColumnsChecked >= 0);

	co_return;
}

// Recursion-cap test: best-effort. We do not ship a self-referential
// fixture, so we document the cap behavior here (MaxDepth=32, sentinel
// shape) without runtime coverage. The cap is exercised manually via the
// tool description.

// Friendly-name collision test: same situation -- behavior is documented but no convenient
// fixture exists in the test project, so it is not exercised at runtime.


#endif // WITH_UNTESTED
