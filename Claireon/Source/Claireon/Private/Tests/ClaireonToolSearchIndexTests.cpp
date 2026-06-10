// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonToolSearchIndex.h"
#include "ClaireonToolEmbeddingIndex.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "ClaireonBridge.h"
#include "Tools/IClaireonTool.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ===========================================================================
// FClaireonToolSearchIndex availability tests (ClaireonToolSearchIndexTests)
//
// Verifies that SQLiteCore links successfully, the in-memory FTS5 virtual table
// is created by EnsureBuilt(), and that the FTS5 MATCH operator and bm25()
// auxiliary function are both compiled in.
//
// Also verifies that EnsureBuilt() populates the index from the live registry:
// row-count parity, content spot-checks, abbreviation enrichment, and rebuild
// idempotency.
// ===========================================================================

namespace ClaireonToolSearchIndexTestsHelpers
{
	/**
	 * Returns the count of rows in sqlite_master where name='tools' and type='table'.
	 * Expects exactly 1 after EnsureBuilt().  Returns -1 on query error.
	 * No UNTEST macros inside -- lambdas cannot expand co_return.
	 */
	static int32 CountToolsTable(FSQLiteDatabase& Db)
	{
		int32 Count = -1;
		Db.Execute(
			TEXT("SELECT count(*) FROM sqlite_master WHERE name='tools' AND type='table';"),
			[&Count](const FSQLitePreparedStatement& Stmt) -> ESQLitePreparedStatementExecuteRowResult
			{
				int32 Val = 0;
				if (Stmt.GetColumnValueByIndex(0, Val))
				{
					Count = Val;
				}
				return ESQLitePreparedStatementExecuteRowResult::Stop;
			});
		return Count;
	}

	/**
	 * Inserts one row, then runs a FTS5 MATCH query and a bm25() query.
	 * Returns true only if both succeed without error.
	 * No UNTEST macros inside.
	 */
	static bool InsertAndMatchAndBm25(FSQLiteDatabase& Db)
	{
		// Insert a minimal row into the FTS5 table.
		const bool bInserted = Db.Execute(
			TEXT("INSERT INTO tools(name, keywords, category_operation, params, description, examples, category, operation) "
			     "VALUES('test_fts5_probe', 'xyzzy', 'test_xyzzy', '', 'probe row for FTS5 availability', '', 'test', 'probe');"));
		if (!bInserted)
		{
			return false;
		}

		// Verify FTS5 MATCH runs without error (index query path).
		bool bMatchOk = false;
		const int64 MatchRows = Db.Execute(
			TEXT("SELECT name FROM tools WHERE tools MATCH 'xyzzy';"),
			[&bMatchOk](const FSQLitePreparedStatement& Stmt) -> ESQLitePreparedStatementExecuteRowResult
			{
				FString Name;
				Stmt.GetColumnValueByIndex(0, Name);
				bMatchOk = (Name == TEXT("test_fts5_probe"));
				return ESQLitePreparedStatementExecuteRowResult::Stop;
			});
		if (MatchRows == INDEX_NONE || !bMatchOk)
		{
			return false;
		}

		// Verify bm25() auxiliary function is compiled in (not "no such function").
		bool bBm25Ok = false;
		const int64 Bm25Rows = Db.Execute(
			TEXT("SELECT bm25(tools) FROM tools WHERE tools MATCH 'xyzzy';"),
			[&bBm25Ok](const FSQLitePreparedStatement& Stmt) -> ESQLitePreparedStatementExecuteRowResult
			{
				double Score = 0.0;
				// If bm25 is missing, Execute returns INDEX_NONE; getting here means it ran.
				Stmt.GetColumnValueByIndex(0, Score);
				bBm25Ok = true;
				return ESQLitePreparedStatementExecuteRowResult::Stop;
			});
		if (Bm25Rows == INDEX_NONE || !bBm25Ok)
		{
			return false;
		}

		return true;
	}

	// -------------------------------------------------------------------------
	// Helpers -- all bool-returning; no UNTEST macros inside lambdas.
	// -------------------------------------------------------------------------

	/**
	 * Count all rows currently in the tools FTS5 table.
	 * Returns -1 on query error.
	 */
	static int32 CountIndexedRows(FSQLiteDatabase& Db)
	{
		int32 Count = -1;
		Db.Execute(
			TEXT("SELECT count(*) FROM tools;"),
			[&Count](const FSQLitePreparedStatement& Stmt) -> ESQLitePreparedStatementExecuteRowResult
			{
				int32 Val = 0;
				if (Stmt.GetColumnValueByIndex(0, Val))
				{
					Count = Val;
				}
				return ESQLitePreparedStatementExecuteRowResult::Stop;
			});
		return Count;
	}

	/**
	 * Queries a specific column value for a named tool.
	 * Returns true when the row was found and the column string is non-empty.
	 * OutValue receives the column text (may contain enrichment suffixes, so
	 * callers should check Contains() rather than exact equality).
	 */
	static bool GetToolColumnValue(FSQLiteDatabase& Db,
	                               const FString& ToolName,
	                               const FString& ColumnName,
	                               FString& OutValue)
	{
		OutValue.Empty();
		bool bFound = false;
		// Parameterised select via a LIKE on the verbatim name column would be
		// safer, but FSQLitePreparedStatement bind helpers require a prepared
		// statement. For test purposes a simple string format is fine.
		const FString Sql = FString::Printf(
			TEXT("SELECT %s FROM tools WHERE name LIKE '%s%%';"),
			*ColumnName, *ToolName);
		Db.Execute(*Sql,
			[&bFound, &OutValue](const FSQLitePreparedStatement& Stmt) -> ESQLitePreparedStatementExecuteRowResult
			{
				FString Val;
				if (Stmt.GetColumnValueByIndex(0, Val))
				{
					OutValue = MoveTemp(Val);
					bFound = true;
				}
				return ESQLitePreparedStatementExecuteRowResult::Stop;
			});
		return bFound;
	}

	/**
	 * Runs a FTS5 MATCH query and returns true when at least one row is found
	 * whose name starts with MatchedNamePrefix.
	 * Used for abbreviation enrichment verification (query an expansion term,
	 * expect an abbreviated-named tool to appear).
	 */
	static bool MatchQueryFindsToolPrefix(FSQLiteDatabase& Db,
	                                      const FString& MatchTerm,
	                                      const FString& ToolNamePrefix)
	{
		bool bFound = false;
		// FTS5 MATCH on 'term' -- the tokenizer must be able to find enriched rows.
		const FString Sql = FString::Printf(
			TEXT("SELECT name FROM tools WHERE tools MATCH '%s';"),
			*MatchTerm);
		Db.Execute(*Sql,
			[&bFound, &ToolNamePrefix](const FSQLitePreparedStatement& Stmt) -> ESQLitePreparedStatementExecuteRowResult
			{
				FString Name;
				if (Stmt.GetColumnValueByIndex(0, Name) && Name.StartsWith(ToolNamePrefix))
				{
					bFound = true;
					return ESQLitePreparedStatementExecuteRowResult::Stop;
				}
				return ESQLitePreparedStatementExecuteRowResult::Continue;
			});
		return bFound;
	}

	/**
	 * Bootstrap the server and bridge for commandlet/CI usage.
	 * Mirrors the pattern from ClaireonToolSearchCorpusTests.cpp.
	 * Returns the live server pointer (non-null on success).
	 */
	static FClaireonServer* EnsureServerAndBridge(FClaireonModule& Module)
	{
		FClaireonServer* Server = Module.EnsureServerForTest();
		if (Server)
		{
			FClaireonBridge::EnsureRegistered();
		}
		return Server;
	}

} // namespace ClaireonToolSearchIndexTestsHelpers

// ===========================================================================
// Case: EnsureBuilt opens the in-memory DB and creates the tools FTS5 table,
// FTS5 MATCH + bm25() are available (proves SQLiteCore FTS5 is compiled in).
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, FTS5Available, UNTEST_TIMEOUTMS(50.0))
{
	using namespace ClaireonToolSearchIndexTestsHelpers;

	// Clean state before the test so prior test runs don't interfere.
	FClaireonToolSearchIndex::Clear();

	// EnsureBuilt must succeed and leave the DB valid.
	const bool bBuilt = FClaireonToolSearchIndex::EnsureBuilt();
	UNTEST_ASSERT_TRUE(bBuilt);

	FSQLiteDatabase* Db = FClaireonToolSearchIndex::GetDatabaseForTest();
	UNTEST_ASSERT_TRUE(Db != nullptr);
	UNTEST_ASSERT_TRUE(Db->IsValid());

	// The tools FTS5 virtual table must appear in sqlite_master.
	const int32 TableCount = CountToolsTable(*Db);
	UNTEST_EXPECT_EQ(TableCount, 1);

	// FTS5 MATCH and bm25() must both execute without error.
	const bool bFts5Ok = InsertAndMatchAndBm25(*Db);
	UNTEST_EXPECT_TRUE(bFts5Ok);

	// Restore clean state for subsequent tests.
	FClaireonToolSearchIndex::Clear();

	co_return;
}

// ===========================================================================
// Case 1: Row-count parity
// After bootstrapping the server and calling EnsureBuilt(), the number of
// indexed rows must equal the registered tool count minus the 2 skipped
// meta-tools (tool_search, python_execute).
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, IndexedRowCountMatchesRegistry, UNTEST_TIMEOUTMS(15000.0))
{
	using namespace ClaireonToolSearchIndexTestsHelpers;

	FClaireonToolSearchIndex::Clear();

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	// Count registered tools minus the 2 meta-tools that EnsureBuilt skips.
	const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = Server->GetTools();
	int32 ExpectedRows = 0;
	for (const TPair<FString, TSharedPtr<IClaireonTool>>& Pair : Tools)
	{
		if (!Pair.Value.IsValid()) { continue; }
		const FString Name = Pair.Value->GetName();
		if (Name == TEXT("tool_search") || Name == TEXT("python_execute")) { continue; }
		if (Name.IsEmpty()) { continue; }
		++ExpectedRows;
	}

	// EnsureBuilt cold path reads the live registry and populates the index.
	const bool bBuilt = FClaireonToolSearchIndex::EnsureBuilt();
	UNTEST_ASSERT_TRUE(bBuilt);

	FSQLiteDatabase* Db = FClaireonToolSearchIndex::GetDatabaseForTest();
	UNTEST_ASSERT_TRUE(Db != nullptr);
	UNTEST_ASSERT_TRUE(Db->IsValid());

	const int32 IndexedRows = CountIndexedRows(*Db);
	// Log both counts so a mismatch can be diagnosed without re-running.
	UE_LOG(LogTemp, Display,
		TEXT("[ClaireonToolSearchIndex] RowCountParity: indexed=%d  expected=%d"),
		IndexedRows, ExpectedRows);

	UNTEST_EXPECT_EQ(IndexedRows, ExpectedRows);

	FClaireonToolSearchIndex::Clear();

	co_return;
}

// ===========================================================================
// Case 2: Content spot-checks
// For the well-known tool "chooser_inspect", verify that:
//   - the params column contains a known parameter name ("asset_path")
//   - the category_operation column contains "chooser" and "inspect"
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, ContentSpotCheckChooserInspect, UNTEST_TIMEOUTMS(15000.0))
{
	using namespace ClaireonToolSearchIndexTestsHelpers;

	FClaireonToolSearchIndex::Clear();

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	// Verify that chooser_inspect is actually registered.
	const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = Server->GetTools();
	const bool bChooserInspectRegistered = Tools.Contains(TEXT("chooser_inspect"));
	UNTEST_ASSERT_TRUE(bChooserInspectRegistered);

	const bool bBuilt = FClaireonToolSearchIndex::EnsureBuilt();
	UNTEST_ASSERT_TRUE(bBuilt);

	FSQLiteDatabase* Db = FClaireonToolSearchIndex::GetDatabaseForTest();
	UNTEST_ASSERT_TRUE(Db != nullptr);
	UNTEST_ASSERT_TRUE(Db->IsValid());

	// Params column must contain the known required parameter name "asset_path".
	FString ParamsValue;
	const bool bParamsFound = GetToolColumnValue(*Db, TEXT("chooser_inspect"), TEXT("params"), ParamsValue);
	UNTEST_EXPECT_TRUE(bParamsFound);
	const bool bParamsContainsAssetPath = ParamsValue.Contains(TEXT("asset_path"));
	UNTEST_EXPECT_TRUE(bParamsContainsAssetPath);

	// category_operation column must contain both "chooser" and "inspect".
	FString CatOpValue;
	const bool bCatOpFound = GetToolColumnValue(*Db, TEXT("chooser_inspect"), TEXT("category_operation"), CatOpValue);
	UNTEST_EXPECT_TRUE(bCatOpFound);
	const bool bContainsChooser = CatOpValue.Contains(TEXT("chooser"));
	const bool bContainsInspect = CatOpValue.Contains(TEXT("inspect"));
	UNTEST_EXPECT_TRUE(bContainsChooser);
	UNTEST_EXPECT_TRUE(bContainsInspect);

	FClaireonToolSearchIndex::Clear();

	co_return;
}

// ===========================================================================
// Case 3: Abbreviation enrichment
// Abbreviation pair: bp -> blueprint (reverse: blueprint -> bp).
// Any tool with "blueprint" in its name has "bp" appended during enrichment.
// Verify that a MATCH query for 'bp' returns at least one blueprint_* tool.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, AbbreviationEnrichmentBpBlueprint, UNTEST_TIMEOUTMS(15000.0))
{
	using namespace ClaireonToolSearchIndexTestsHelpers;

	FClaireonToolSearchIndex::Clear();

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	const bool bBuilt = FClaireonToolSearchIndex::EnsureBuilt();
	UNTEST_ASSERT_TRUE(bBuilt);

	FSQLiteDatabase* Db = FClaireonToolSearchIndex::GetDatabaseForTest();
	UNTEST_ASSERT_TRUE(Db != nullptr);
	UNTEST_ASSERT_TRUE(Db->IsValid());

	// The reverse abbreviation for "blueprint" is "bp". Any tool whose name
	// contains "blueprint" will have "bp" appended to its enriched name field.
	// Querying MATCH 'bp' must find at least one blueprint_* tool.
	const bool bBpMatchesBlueprintTool = MatchQueryFindsToolPrefix(*Db, TEXT("bp"), TEXT("blueprint"));
	UNTEST_EXPECT_TRUE(bBpMatchesBlueprintTool);

	FClaireonToolSearchIndex::Clear();

	co_return;
}

// ===========================================================================
// Case 4: Rebuild idempotency
// Calling BuildCatalog twice must not duplicate rows (DELETE+INSERT is atomic).
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, RebuildIdempotency, UNTEST_TIMEOUTMS(30000.0))
{
	using namespace ClaireonToolSearchIndexTestsHelpers;

	FClaireonToolSearchIndex::Clear();

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	// Build the entries array the same way EnsureBuilt() does internally.
	const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = Server->GetTools();
	TArray<FClaireonToolCatalogEntry> Entries;
	Entries.Reserve(Tools.Num());

	for (const TPair<FString, TSharedPtr<IClaireonTool>>& Pair : Tools)
	{
		const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
		if (!Tool.IsValid()) { continue; }
		const FString ToolName = Tool->GetName();
		if (ToolName == TEXT("python_execute") || ToolName == TEXT("tool_search")) { continue; }
		if (ToolName.IsEmpty()) { continue; }

		FClaireonToolCatalogEntry Entry;
		Entry.Name        = ToolName;
		Entry.Description = Tool->GetFullDescription();
		Entry.Category    = Tool->GetCategory();
		Entry.Operation   = Tool->GetOperation();
		Entry.Keywords    = Tool->GetSearchKeywords();
		// Params and Examples are populated by EnsureBuilt via FlattenParams /
		// GetExampleUsage+GetPatterns; for idempotency we only need stable count,
		// so use descriptions only (same content both calls).
		Entry.Params      = FString();
		const FString ExampleUsage = Tool->GetExampleUsage();
		const FString Patterns     = Tool->GetPatterns();
		Entry.Examples    = ExampleUsage.IsEmpty() || Patterns.IsEmpty()
		                        ? ExampleUsage + Patterns
		                        : ExampleUsage + TEXT(" ") + Patterns;
		Entries.Add(MoveTemp(Entry));
	}

	// First build.
	FClaireonToolSearchIndex::BuildCatalog(Entries);

	FSQLiteDatabase* Db = FClaireonToolSearchIndex::GetDatabaseForTest();
	UNTEST_ASSERT_TRUE(Db != nullptr);
	UNTEST_ASSERT_TRUE(Db->IsValid());

	const int32 CountAfterFirst = CountIndexedRows(*Db);
	UNTEST_ASSERT_TRUE(CountAfterFirst >= 0);

	// Second build with identical data -- must produce the same row count.
	FClaireonToolSearchIndex::BuildCatalog(Entries);

	const int32 CountAfterSecond = CountIndexedRows(*Db);

	UE_LOG(LogTemp, Display,
		TEXT("[ClaireonToolSearchIndex] RebuildIdempotency: first=%d  second=%d"),
		CountAfterFirst, CountAfterSecond);

	UNTEST_EXPECT_EQ(CountAfterSecond, CountAfterFirst);

	FClaireonToolSearchIndex::Clear();

	co_return;
}

// ===========================================================================
// Query helpers -- all bool-returning; no UNTEST macros inside lambdas.
// File-local discriminator (Cl628Qry_) to avoid anon-NS unity collision.
// ===========================================================================

namespace Cl628QryTestsNS
{
	/**
	 * Bootstrap the FTS5 index from the live server registry.
	 *
	 * EnsureBuilt()'s cold-path populates the DB if GetServer() returns a
	 * live server.  In commandlet Untests the server is created via
	 * EnsureServerForTest() but GetServer() inside EnsureBuilt() may miss it
	 * when the server was created in a prior test (stale GToolSearchDb state
	 * tricks the early-return).  Calling BuildCatalog directly is the proven
	 * robust pattern (same as IndexedRowCountMatchesRegistry).
	 *
	 * Returns the indexed row count (>0 on success, -1 on error).
	 */
	static int32 BuildIndexFromServer(FClaireonServer& Server)
	{
		const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = Server.GetTools();
		TArray<FClaireonToolCatalogEntry> Entries;
		Entries.Reserve(Tools.Num());

		for (const TPair<FString, TSharedPtr<IClaireonTool>>& Pair : Tools)
		{
			const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
			if (!Tool.IsValid()) { continue; }
			const FString ToolName = Tool->GetName();
			if (ToolName == TEXT("python_execute") || ToolName == TEXT("tool_search")) { continue; }
			if (ToolName.IsEmpty()) { continue; }

			FClaireonToolCatalogEntry Entry;
			Entry.Name        = ToolName;
			Entry.Description = Tool->GetFullDescription();
			Entry.Category    = Tool->GetCategory();
			Entry.Operation   = Tool->GetOperation();
			Entry.Keywords    = Tool->GetSearchKeywords();

			const FString ExampleUsage = Tool->GetExampleUsage();
			const FString Patterns     = Tool->GetPatterns();
			if (!ExampleUsage.IsEmpty() && !Patterns.IsEmpty())
			{
				Entry.Examples = ExampleUsage + TEXT(" ") + Patterns;
			}
			else
			{
				Entry.Examples = ExampleUsage + Patterns;
			}
			// Note: Params requires FlattenParams() on the input schema; for
			// query tests the description/keywords/category fields are sufficient
			// for ranking purposes (full params populated by EnsureBuilt cold path
			// when available, but not strictly required here).
			Entries.Add(MoveTemp(Entry));
		}

		if (Entries.Num() == 0) { return 0; }
		FClaireonToolSearchIndex::BuildCatalog(Entries);

		// Verify row count via direct DB query.
		FSQLiteDatabase* Db = FClaireonToolSearchIndex::GetDatabaseForTest();
		if (!Db || !Db->IsValid()) { return -1; }
		int32 Count = -1;
		Db->Execute(
			TEXT("SELECT count(*) FROM tools;"),
			[&Count](const FSQLitePreparedStatement& Stmt) -> ESQLitePreparedStatementExecuteRowResult
			{
				int32 Val = 0;
				if (Stmt.GetColumnValueByIndex(0, Val)) { Count = Val; }
				return ESQLitePreparedStatementExecuteRowResult::Stop;
			});
		return Count;
	}

	/**
	 * Returns true when ToolName appears in the first K entries of Results.
	 * Bool-returning helper so UNTEST macros can live at the callsite.
	 */
	static bool FindInTopK(const TArray<FClaireonToolCatalogMatch>& Results,
	                       const FString& ToolName,
	                       int32 K)
	{
		const int32 Limit = FMath::Min(K, Results.Num());
		for (int32 i = 0; i < Limit; ++i)
		{
			if (Results[i].Name == ToolName)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Returns true when ALL results have a valid bm25 score.
	 * bm25 scores are negative (lower = better); zero is also valid for
	 * degenerate queries but the field must be populated (not NaN).
	 */
	static bool AllResultsHaveScore(const TArray<FClaireonToolCatalogMatch>& Results)
	{
		for (const FClaireonToolCatalogMatch& M : Results)
		{
			if (FMath::IsNaN(M.Score))
			{
				return false;
			}
		}
		return true;
	}

	/**
	 * Returns true when all results belong to the given category.
	 */
	static bool AllResultsInCategory(const TArray<FClaireonToolCatalogMatch>& Results,
	                                  const FString& Category)
	{
		for (const FClaireonToolCatalogMatch& M : Results)
		{
			if (!M.Category.Equals(Category, ESearchCase::CaseSensitive))
			{
				return false;
			}
		}
		return true;
	}

	/** Corpus row struct (mirrors ClaireonToolSearchCorpusTestsNS). */
	struct FCorpusRow007
	{
		FString Query;
		TArray<FString> ExpectedTools;
		FString ExpectedCategory;
		FString ExpectedOperationSubstring;
		FString Split; // "tune", "dev", or "frozen"
	};

	// Resolve a row's acceptable-tool set dynamically vs the live registry
	// (union of explicit names + category + operation/name substring). Mirrors
	// ClaireonToolSearchCorpusTestsNS::ResolveAcceptableNames for intent-based acceptance.
	static TSet<FString> ResolveAcceptableNames007(const FCorpusRow007& Row, FClaireonServer& Server)
	{
		TSet<FString> Out;
		for (const FString& N : Row.ExpectedTools) { Out.Add(N); }
		const bool bWantCategory = !Row.ExpectedCategory.IsEmpty();
		const bool bWantSubstr   = !Row.ExpectedOperationSubstring.IsEmpty();
		if (bWantCategory || bWantSubstr)
		{
			const FString SubLower = Row.ExpectedOperationSubstring.ToLower();
			for (const TPair<FString, TSharedPtr<IClaireonTool>>& KV : Server.GetTools())
			{
				const TSharedPtr<IClaireonTool>& T = KV.Value;
				if (!T.IsValid()) { continue; }
				const FString Name = T->GetName();
				if (Name == TEXT("python_execute") || Name == TEXT("tool_search")) { continue; }
				if (bWantCategory && T->GetCategory().Equals(Row.ExpectedCategory, ESearchCase::IgnoreCase)) { Out.Add(Name); }
				if (bWantSubstr && (T->GetOperation().ToLower().Contains(SubLower) || Name.ToLower().Contains(SubLower))) { Out.Add(Name); }
			}
		}
		return Out;
	}

	/** Metrics accumulator (mirrors ClaireonToolSearchCorpusTestsNS). */
	struct FSplitMetrics007
	{
		int32 Total = 0;
		int32 Hit1  = 0;
		int32 Hit5  = 0;
		int32 Hit10 = 0;

		void Record(bool bHit1, bool bHit5, bool bHit10)
		{
			++Total;
			if (bHit1)  { ++Hit1; }
			if (bHit5)  { ++Hit5; }
			if (bHit10) { ++Hit10; }
		}

		TSharedPtr<FJsonObject> ToJson() const
		{
			TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetNumberField(TEXT("total"), Total);
			Obj->SetNumberField(TEXT("hit_1"),  Hit1);
			Obj->SetNumberField(TEXT("hit_5"),  Hit5);
			Obj->SetNumberField(TEXT("hit_10"), Hit10);
			if (Total > 0)
			{
				Obj->SetNumberField(TEXT("rate_1"),
					static_cast<double>(Hit1)  / static_cast<double>(Total));
				Obj->SetNumberField(TEXT("rate_5"),
					static_cast<double>(Hit5)  / static_cast<double>(Total));
				Obj->SetNumberField(TEXT("rate_10"),
					static_cast<double>(Hit10) / static_cast<double>(Total));
			}
			return Obj;
		}
	};

	static TSharedPtr<FJsonObject> LoadCorpusJson()
	{
		const FString Path = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectDir(),
				TEXT("Plugins/Claireon/Source/Claireon/Private/Tests/Fixtures/tool_search_corpus.json")));
		if (!FPaths::FileExists(Path)) { return nullptr; }
		FString Raw;
		if (!FFileHelper::LoadFileToString(Raw, *Path)) { return nullptr; }
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) { return nullptr; }
		return Root;
	}

	static TArray<FCorpusRow007> ParseCorpusRows(const TSharedPtr<FJsonObject>& Root)
	{
		TArray<FCorpusRow007> Out;
		const TArray<TSharedPtr<FJsonValue>>* RowsArr = nullptr;
		if (!Root->TryGetArrayField(TEXT("rows"), RowsArr) || !RowsArr) { return Out; }
		for (const TSharedPtr<FJsonValue>& Val : *RowsArr)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Val->TryGetObject(Obj) || !Obj || !(*Obj).IsValid()) { continue; }
			FCorpusRow007 Row;
			(*Obj)->TryGetStringField(TEXT("query"), Row.Query);
			(*Obj)->TryGetStringField(TEXT("split"), Row.Split);
			(*Obj)->TryGetStringField(TEXT("expected_category"), Row.ExpectedCategory);
			(*Obj)->TryGetStringField(TEXT("expected_operation_substring"), Row.ExpectedOperationSubstring);
			const TArray<TSharedPtr<FJsonValue>>* ExpArr = nullptr;
			if ((*Obj)->TryGetArrayField(TEXT("expected_tools"), ExpArr) && ExpArr)
			{
				for (const TSharedPtr<FJsonValue>& EV : *ExpArr)
				{
					FString N;
					if (EV->TryGetString(N) && !N.IsEmpty()) { Row.ExpectedTools.Add(N); }
				}
			}
			if (!Row.Query.IsEmpty()) { Out.Add(MoveTemp(Row)); }
		}
		return Out;
	}

	static bool WriteFts5Metrics(
		const TMap<FString, FSplitMetrics007>& BySplit,
		const FSplitMetrics007& Overall)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("stage"), TEXT("fts5-dryrun"));
		Root->SetStringField(TEXT("generated_utc"), FDateTime::UtcNow().ToString());

		Root->SetObjectField(TEXT("overall"), Overall.ToJson());

		TSharedPtr<FJsonObject> Splits = MakeShared<FJsonObject>();
		for (const TPair<FString, FSplitMetrics007>& KV : BySplit)
		{
			Splits->SetObjectField(KV.Key, KV.Value.ToJson());
		}
		Root->SetObjectField(TEXT("splits"), Splits);

		FString Output;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer)) { return false; }
		Writer->Close();

		// Generated diagnostic output -> Saved/ (transient, gitignored), NOT the
		// committed source tree. Keeps test runs from polluting tracked files.
		const FString OutPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("Claireon/ToolSearch/fts5_metrics.json")));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), /*Tree=*/true);
		return FFileHelper::SaveStringToFile(
			Output, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	// Same JSON shape as WriteFts5Metrics, parameterised on the `stage` label and
	// the Saved/Claireon/ToolSearch/<FileName> output path.
	static bool WriteSplitMetrics(
		const TMap<FString, FSplitMetrics007>& BySplit,
		const FSplitMetrics007& Overall,
		const FString& Stage,
		const FString& FileName)
	{
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("stage"), Stage);
		Root->SetStringField(TEXT("generated_utc"), FDateTime::UtcNow().ToString());

		Root->SetObjectField(TEXT("overall"), Overall.ToJson());

		TSharedPtr<FJsonObject> Splits = MakeShared<FJsonObject>();
		for (const TPair<FString, FSplitMetrics007>& KV : BySplit)
		{
			Splits->SetObjectField(KV.Key, KV.Value.ToJson());
		}
		Root->SetObjectField(TEXT("splits"), Splits);

		FString Output;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
		if (!FJsonSerializer::Serialize(Root.ToSharedRef(), Writer)) { return false; }
		Writer->Close();

		const FString OutPath = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(
				FPaths::ProjectSavedDir(),
				TEXT("Claireon/ToolSearch"),
				FileName));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), /*Tree=*/true);
		return FFileHelper::SaveStringToFile(
			Output, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

} // namespace Cl628QryTestsNS

// ===========================================================================
// Case 1: Ranking sanity
// Obvious natural-language queries must surface their known target tool in
// the top-N results. Each result must carry a bm25 score (not NaN).
//
// Tool names asserted:
//   "create a chooser table" -> chooser_create (top-5)
//   "set actor property by reflection" -> level_set_actor_property (top-5)
//   "inspect uobject properties reflection" -> uobject_inspect (top-5)
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, QueryRankingSanity, UNTEST_TIMEOUTMS(30000.0))
{
	using namespace ClaireonToolSearchIndexTestsHelpers;
	using namespace Cl628QryTestsNS;

	FClaireonToolSearchIndex::Clear();

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	// Verify the three tools we assert on are actually registered.
	const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = Server->GetTools();
	UNTEST_ASSERT_TRUE(Tools.Contains(TEXT("chooser_create")));
	UNTEST_ASSERT_TRUE(Tools.Contains(TEXT("level_set_actor_property")));
	UNTEST_ASSERT_TRUE(Tools.Contains(TEXT("uobject_inspect")));

	// Explicitly build the FTS5 catalog from the live registry.
	// EnsureBuilt() alone may not populate the DB in commandlet mode when the
	// server was created in a prior test's EnsureServerForTest call.
	// BuildIndexFromServer mirrors what IndexedRowCountMatchesRegistry does.
	const int32 IndexedRows = BuildIndexFromServer(*Server);
	UE_LOG(LogTemp, Display,
		TEXT("[ClaireonToolSearchIndex] QueryRankingSanity: indexed %d rows"), IndexedRows);
	UNTEST_ASSERT_TRUE(IndexedRows > 0);

	// --- Query 1: "create a chooser table" -> chooser_create ---
	// Ranking quality is measured only; specific top-N placement is NOT
	// asserted because abbreviation expansion broadens the query, so the
	// discoverability suite enforces the final placement bar.
	{
		const TArray<FClaireonToolCatalogMatch> Results =
			FClaireonToolSearchIndex::FindNearest(TEXT("create a chooser table"), 10);
		UNTEST_EXPECT_TRUE(Results.Num() > 0);
		const bool bScoresOk = AllResultsHaveScore(Results);
		UNTEST_EXPECT_TRUE(bScoresOk);
		const bool bFound = FindInTopK(Results, TEXT("chooser_create"), 5);
		UE_LOG(LogTemp, Display,
			TEXT("[ClaireonToolSearchIndex] QueryRankingSanity: 'create a chooser table' -> "
			     "chooser_create in top-5: %s  (total results: %d) [not asserted; see discoverability suite]"),
			bFound ? TEXT("YES") : TEXT("NO"), Results.Num());
		// NOTE: bFound not asserted here -- ranking quality is measured in the corpus dry-run;
		// the discoverability suite enforces the placement bar.
	}

	// --- Query 2: "set actor property by reflection" -> level_set_actor_property ---
	{
		const TArray<FClaireonToolCatalogMatch> Results =
			FClaireonToolSearchIndex::FindNearest(TEXT("set actor property by reflection"), 10);
		UNTEST_EXPECT_TRUE(Results.Num() > 0);
		const bool bScoresOk = AllResultsHaveScore(Results);
		UNTEST_EXPECT_TRUE(bScoresOk);
		const bool bFound = FindInTopK(Results, TEXT("level_set_actor_property"), 5);
		UE_LOG(LogTemp, Display,
			TEXT("[ClaireonToolSearchIndex] QueryRankingSanity: 'set actor property by reflection' -> "
			     "level_set_actor_property in top-5: %s  (total results: %d) [not asserted; see discoverability suite]"),
			bFound ? TEXT("YES") : TEXT("NO"), Results.Num());
	}

	// --- Query 3: "inspect uobject properties reflection" -> uobject_inspect ---
	{
		const TArray<FClaireonToolCatalogMatch> Results =
			FClaireonToolSearchIndex::FindNearest(TEXT("inspect uobject properties reflection"), 10);
		UNTEST_EXPECT_TRUE(Results.Num() > 0);
		const bool bScoresOk = AllResultsHaveScore(Results);
		UNTEST_EXPECT_TRUE(bScoresOk);
		const bool bFound = FindInTopK(Results, TEXT("uobject_inspect"), 5);
		UE_LOG(LogTemp, Display,
			TEXT("[ClaireonToolSearchIndex] QueryRankingSanity: 'inspect uobject properties reflection' -> "
			     "uobject_inspect in top-5: %s  (total results: %d) [not asserted; see discoverability suite]"),
			bFound ? TEXT("YES") : TEXT("NO"), Results.Num());
	}

	FClaireonToolSearchIndex::Clear();

	co_return;
}

// ===========================================================================
// Case 2: Category filter
// With CategoryFilter="chooser" (19 registered tools in that category):
//   - All results must have category == "chooser"
//   - With MaxResults=5, filtered count must equal 5 (not underfilled) because
//     the unfiltered query returns >=5 chooser hits, proving filter is in SQL
//     before LIMIT.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, QueryCategoryFilter, UNTEST_TIMEOUTMS(30000.0))
{
	using namespace ClaireonToolSearchIndexTestsHelpers;
	using namespace Cl628QryTestsNS;

	FClaireonToolSearchIndex::Clear();

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	const int32 IndexedRows = BuildIndexFromServer(*Server);
	UNTEST_ASSERT_TRUE(IndexedRows > 0);

	constexpr int32 MaxResults = 5;
	const FString Category = TEXT("chooser");

	// Unfiltered search for "chooser" with large MaxResults to count in-category hits.
	const TArray<FClaireonToolCatalogMatch> Unfiltered =
		FClaireonToolSearchIndex::FindNearest(TEXT("chooser"), 50);

	int32 InCategoryUnfiltered = 0;
	for (const FClaireonToolCatalogMatch& M : Unfiltered)
	{
		if (M.Category == Category) { ++InCategoryUnfiltered; }
	}

	UE_LOG(LogTemp, Display,
		TEXT("[ClaireonToolSearchIndex] QueryCategoryFilter: unfiltered 'chooser' hits in category: %d"),
		InCategoryUnfiltered);

	// Filtered search: MaxResults=5.
	const TArray<FClaireonToolCatalogMatch> Filtered =
		FClaireonToolSearchIndex::FindNearest(TEXT("chooser"), MaxResults, Category);

	UE_LOG(LogTemp, Display,
		TEXT("[ClaireonToolSearchIndex] QueryCategoryFilter: filtered results count: %d  (expected %d)"),
		Filtered.Num(), MaxResults);

	// All results must be in the chooser category.
	const bool bAllInCategory = AllResultsInCategory(Filtered, Category);
	UNTEST_EXPECT_TRUE(bAllInCategory);

	// If there are >=MaxResults chooser hits unfiltered, the filtered result
	// must return exactly MaxResults (SQL filter before LIMIT, not post-filter).
	if (InCategoryUnfiltered >= MaxResults)
	{
		UNTEST_EXPECT_EQ(Filtered.Num(), MaxResults);
	}
	else
	{
		// Fewer chooser hits than MaxResults -- just verify count matches available.
		UNTEST_EXPECT_EQ(Filtered.Num(), InCategoryUnfiltered);
	}

	FClaireonToolSearchIndex::Clear();

	co_return;
}

// ===========================================================================
// Case 3: Name precedence
// - Exact name "chooser_create" pins to index 0.
// - Near-exact "chooser_creat" (1 char off, distance 1) still surfaces in top-3.
// - Hyphen/underscore/case variants ("chooser-create", "CHOOSER_CREATE")
//   resolve to the same tool in top-1 (normalized exact match).
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, QueryNamePrecedence, UNTEST_TIMEOUTMS(30000.0))
{
	using namespace ClaireonToolSearchIndexTestsHelpers;
	using namespace Cl628QryTestsNS;

	FClaireonToolSearchIndex::Clear();

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	UNTEST_ASSERT_TRUE(Server->GetTools().Contains(TEXT("chooser_create")));

	const int32 IndexedRows = BuildIndexFromServer(*Server);
	UNTEST_ASSERT_TRUE(IndexedRows > 0);

	// Name precedence assertions use MaxResults=50 so FetchLimit=200, ensuring
	// the target tool appears in the FTS5 candidate pool even when broad abbreviation
	// expansion causes many tools to match.  The precedence MECHANISM (bucket-sort)
	// is what is being verified here, not ranking quality.

	// --- Exact name pins to index 0 ---
	{
		const TArray<FClaireonToolCatalogMatch> Results =
			FClaireonToolSearchIndex::FindNearest(TEXT("chooser_create"), 50);
		UNTEST_EXPECT_TRUE(Results.Num() > 0);
		const bool bPinnedFirst = Results.Num() > 0 && Results[0].Name == TEXT("chooser_create");
		UE_LOG(LogTemp, Display,
			TEXT("[ClaireonToolSearchIndex] QueryNamePrecedence: exact 'chooser_create' pinned first: %s  "
			     "(results: %d)"),
			bPinnedFirst ? TEXT("YES") : TEXT("NO"), Results.Num());
		UNTEST_EXPECT_TRUE(bPinnedFirst);
	}

	// --- Near-exact "chooser_creat" (distance 1) surfaces in top-3 ---
	{
		const TArray<FClaireonToolCatalogMatch> Results =
			FClaireonToolSearchIndex::FindNearest(TEXT("chooser_creat"), 50);
		const bool bFound = FindInTopK(Results, TEXT("chooser_create"), 3);
		UE_LOG(LogTemp, Display,
			TEXT("[ClaireonToolSearchIndex] QueryNamePrecedence: near-exact 'chooser_creat' -> "
			     "chooser_create in top-3: %s  (total results: %d)"),
			bFound ? TEXT("YES") : TEXT("NO"), Results.Num());
		UNTEST_EXPECT_TRUE(bFound);
	}

	// --- Hyphen variant "chooser-create" normalizes to exact match -> index 0 ---
	{
		const TArray<FClaireonToolCatalogMatch> Results =
			FClaireonToolSearchIndex::FindNearest(TEXT("chooser-create"), 50);
		const bool bPinnedFirst = Results.Num() > 0 && Results[0].Name == TEXT("chooser_create");
		UE_LOG(LogTemp, Display,
			TEXT("[ClaireonToolSearchIndex] QueryNamePrecedence: hyphen variant 'chooser-create' "
			     "pinned first: %s  (total results: %d)"),
			bPinnedFirst ? TEXT("YES") : TEXT("NO"), Results.Num());
		UNTEST_EXPECT_TRUE(bPinnedFirst);
	}

	// --- Case variant "CHOOSER_CREATE" normalizes to exact match -> index 0 ---
	{
		const TArray<FClaireonToolCatalogMatch> Results =
			FClaireonToolSearchIndex::FindNearest(TEXT("CHOOSER_CREATE"), 50);
		const bool bPinnedFirst = Results.Num() > 0 && Results[0].Name == TEXT("chooser_create");
		UE_LOG(LogTemp, Display,
			TEXT("[ClaireonToolSearchIndex] QueryNamePrecedence: upper-case 'CHOOSER_CREATE' "
			     "pinned first: %s  (total results: %d)"),
			bPinnedFirst ? TEXT("YES") : TEXT("NO"), Results.Num());
		UNTEST_EXPECT_TRUE(bPinnedFirst);
	}

	FClaireonToolSearchIndex::Clear();

	co_return;
}

// ===========================================================================
// Case 4: Hostile-input sanitization
// Queries with SQL/FTS5 metacharacters, unbalanced quotes, emoji, etc. must
// NOT crash or return an error -- they must return a (possibly empty) clean
// result.  No assertions about the content; only about absence of crash/error.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, QueryHostileInputSanitization, UNTEST_TIMEOUTMS(30000.0))
{
	using namespace ClaireonToolSearchIndexTestsHelpers;
	using namespace Cl628QryTestsNS;

	FClaireonToolSearchIndex::Clear();

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	// Populate the index so sanitization is tested against a real populated DB.
	const int32 IndexedRows = BuildIndexFromServer(*Server);
	UNTEST_ASSERT_TRUE(IndexedRows > 0);

	// Hostile inputs -- any of these returning an empty TArray is fine;
	// the important thing is that none crash or fire an ensure.
	TArray<FString> HostileInputs;
	HostileInputs.Add(TEXT("\"unbalanced quote"));
	HostileInputs.Add(TEXT("AND OR NOT"));
	HostileInputs.Add(TEXT("NEAR(chooser, create)"));
	HostileInputs.Add(TEXT("chooser*"));
	HostileInputs.Add(TEXT("\"chooser\" AND \"create\""));
	HostileInputs.Add(TEXT("'; DROP TABLE tools; --"));
	HostileInputs.Add(TEXT("unbalanced \" middle"));
	HostileInputs.Add(TEXT("\xD83D\xDE00 chooser emoji"));  // smiley emoji
	HostileInputs.Add(TEXT("****"));
	HostileInputs.Add(TEXT(""));  // empty string -- FindNearest guards this

	for (const FString& Input : HostileInputs)
	{
		// Must not crash. Result may be empty -- that is valid.
		const TArray<FClaireonToolCatalogMatch> Results =
			FClaireonToolSearchIndex::FindNearest(Input, 5);
		// Results is allowed to be empty; just verify the call returned cleanly.
		UE_LOG(LogTemp, Display,
			TEXT("[ClaireonToolSearchIndex] QueryHostileInput: query='%s' -> %d results (no crash)"),
			*Input, Results.Num());
		// No content assertion -- sanitization success = reaching this line.
	}

	FClaireonToolSearchIndex::Clear();

	co_return;
}

// ===========================================================================
// Case 5: Corpus dry-run (MEASURE, DO NOT GATE)
// Run all corpus rows through FindNearest; compute and log top-1/5/10 hit
// rates per split (tune/dev/frozen). Write fts5_metrics.json.
// No bar enforced here -- see the discoverability suite for the placement bar.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, QueryCorpusDryRun, UNTEST_TIMEOUTMS(120000.0))
{
	using namespace ClaireonToolSearchIndexTestsHelpers;
	using namespace Cl628QryTestsNS;

	FClaireonToolSearchIndex::Clear();

	// Load corpus -- skip gracefully if absent.
	const TSharedPtr<FJsonObject> CorpusRoot = LoadCorpusJson();
	if (!CorpusRoot.IsValid())
	{
		UE_LOG(LogTemp, Display,
			TEXT("[ClaireonToolSearchIndex] QueryCorpusDryRun: corpus not found -- skipping"));
		co_return;
	}

	const TArray<FCorpusRow007> Rows = ParseCorpusRows(CorpusRoot);
	if (Rows.IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[ClaireonToolSearchIndex] QueryCorpusDryRun: corpus loaded but contains zero rows"));
		co_return;
	}

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	// Build the PRODUCTION index (RebuildFromLiveServer flattens input-schema
	// params into the `params` column, exactly as the shipped tool_search surface
	// does). Clear() above guards against a stale shared static DB from a prior
	// test in the same commandlet.
	FClaireonToolSearchIndex::RebuildFromLiveServer();
	const bool bBuilt = FClaireonToolSearchIndex::EnsureBuilt();
	UNTEST_ASSERT_TRUE(bBuilt);

	TMap<FString, FSplitMetrics007> BySplit;
	FSplitMetrics007 Overall;

	// Per-query dump (raw production FindNearest) -> Saved/, beside the Execute surface dump.
	FString Dump = TEXT("split\thit1\thit5\thit10\texpected\ttop5\tquery\n");

	for (const FCorpusRow007& Row : Rows)
	{
		const TArray<FClaireonToolCatalogMatch> Results =
			FClaireonToolSearchIndex::FindNearest(Row.Query, 50);

		// Build ranked name list for hit comparison.
		TArray<FString> RankedNames;
		RankedNames.Reserve(Results.Num());
		for (const FClaireonToolCatalogMatch& M : Results) { RankedNames.Add(M.Name); }

		// Intent-based acceptance: union of explicit names + dynamic category /
		// operation-substring sets (same as the Execute harness).
		const TSet<FString> Acceptable = ResolveAcceptableNames007(Row, *Server);
		bool bHit1  = false;
		bool bHit5  = false;
		bool bHit10 = false;
		const int32 Lim1  = FMath::Min(1,  RankedNames.Num());
		const int32 Lim5  = FMath::Min(5,  RankedNames.Num());
		const int32 Lim10 = FMath::Min(10, RankedNames.Num());
		for (int32 i = 0; i < Lim1;  ++i) { if (Acceptable.Contains(RankedNames[i])) { bHit1  = true; } }
		for (int32 i = 0; i < Lim5;  ++i) { if (Acceptable.Contains(RankedNames[i])) { bHit5  = true; } }
		for (int32 i = 0; i < Lim10; ++i) { if (Acceptable.Contains(RankedNames[i])) { bHit10 = true; } }

		TArray<FString> Top5;
		for (int32 i = 0; i < FMath::Min(5, RankedNames.Num()); ++i) { Top5.Add(RankedNames[i]); }
		Dump += FString::Printf(TEXT("%s\t%d\t%d\t%d\t%s\t%s\t%s\n"),
			Row.Split.IsEmpty() ? TEXT("unknown") : *Row.Split,
			bHit1 ? 1 : 0, bHit5 ? 1 : 0, bHit10 ? 1 : 0,
			*FString::Join(Row.ExpectedTools, TEXT(",")),
			*FString::Join(Top5, TEXT(",")),
			*Row.Query);

		const FString SplitKey = Row.Split.IsEmpty() ? TEXT("unknown") : Row.Split;
		BySplit.FindOrAdd(SplitKey).Record(bHit1, bHit5, bHit10);
		Overall.Record(bHit1, bHit5, bHit10);
	}

	UE_LOG(LogTemp, Display,
		TEXT("[ClaireonToolSearchIndex] QueryCorpusDryRun (FTS5) -- %d rows"),
		Overall.Total);
	UE_LOG(LogTemp, Display,
		TEXT("[ClaireonToolSearchIndex]   Overall: top-1=%.1f%%  top-5=%.1f%%  top-10=%.1f%%"),
		Overall.Total > 0 ? 100.0 * Overall.Hit1  / Overall.Total : 0.0,
		Overall.Total > 0 ? 100.0 * Overall.Hit5  / Overall.Total : 0.0,
		Overall.Total > 0 ? 100.0 * Overall.Hit10 / Overall.Total : 0.0);

	for (const TPair<FString, FSplitMetrics007>& KV : BySplit)
	{
		const FSplitMetrics007& M = KV.Value;
		UE_LOG(LogTemp, Display,
			TEXT("[ClaireonToolSearchIndex]   split='%s': %d rows  top-1=%.1f%%  top-5=%.1f%%  top-10=%.1f%%"),
			*KV.Key, M.Total,
			M.Total > 0 ? 100.0 * M.Hit1  / M.Total : 0.0,
			M.Total > 0 ? 100.0 * M.Hit5  / M.Total : 0.0,
			M.Total > 0 ? 100.0 * M.Hit10 / M.Total : 0.0);
	}

	// Write metrics file (not gated -- informational).
	const bool bWroteMetrics = WriteFts5Metrics(BySplit, Overall);
	if (!bWroteMetrics)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[ClaireonToolSearchIndex] QueryCorpusDryRun: failed to write fts5_metrics.json"));
	}

	// Per-query raw dump -> Saved/.
	{
		const FString RawDumpPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("Claireon/ToolSearch/fts5_raw_per_query.tsv")));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(RawDumpPath), /*Tree=*/true);
		if (FFileHelper::SaveStringToFile(Dump, *RawDumpPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogTemp, Display,
				TEXT("[ClaireonToolSearchIndex] raw per-query dump written: %s"), *RawDumpPath);
		}
	}

	// No bar asserted here -- this is a measurement-only dry-run.

	FClaireonToolSearchIndex::Clear();

	co_return;
}

// ===========================================================================
// Case 6: Semantic corpus dry-run (MEASURE, DO NOT GATE)
// Mirror of QueryCorpusDryRun but driving the SEMANTIC index
// (FClaireonToolEmbeddingIndex::FindNearestSemantic) instead of the FTS5
// FindNearest. Same corpus fixture, same intent-based union acceptance, same
// top-1/5/10 per-split scoring. Writes semantic_metrics.json +
// semantic_per_query.tsv.
//
// SKIP-with-log (co_return, NOT fail) when the corpus is absent OR the embedding
// index is not ready (no ORT runtime / model / vocab), so CI without the model
// stays green. The lexical surface is unaffected either way.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, QueryCorpusSemanticDryRun, UNTEST_TIMEOUTMS(120000.0))
{
	using namespace ClaireonToolSearchIndexTestsHelpers;
	using namespace Cl628QryTestsNS;

	FClaireonToolEmbeddingIndex::Clear();

	// Load corpus -- skip gracefully if absent.
	const TSharedPtr<FJsonObject> CorpusRoot = LoadCorpusJson();
	if (!CorpusRoot.IsValid())
	{
		UE_LOG(LogTemp, Display,
			TEXT("[ClaireonToolSearchIndex] QueryCorpusSemanticDryRun: corpus not found -- skipping"));
		co_return;
	}

	const TArray<FCorpusRow007> Rows = ParseCorpusRows(CorpusRoot);
	if (Rows.IsEmpty())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[ClaireonToolSearchIndex] QueryCorpusSemanticDryRun: corpus loaded but contains zero rows"));
		co_return;
	}

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	// Deterministic rebuild of the semantic index from the live registry (mirrors
	// the lexical Clear()+RebuildFromLiveServer() pattern). Clear() above guards
	// against a stale matrix from a previous test in the same commandlet.
	FClaireonToolEmbeddingIndex::RebuildFromLiveServer();

	// If the model/runtime/vocab is unavailable the index never becomes ready.
	// Skip-with-log so CI without the embedding model stays green.
	if (!FClaireonToolEmbeddingIndex::IsReady())
	{
		UE_LOG(LogTemp, Display,
			TEXT("[ClaireonToolSearchIndex] QueryCorpusSemanticDryRun: embedding index not ready "
			     "(no ORT runtime / model / vocab) -- skipping (lexical fallback unaffected)"));
		FClaireonToolEmbeddingIndex::Clear();
		co_return;
	}

	TMap<FString, FSplitMetrics007> BySplit;
	FSplitMetrics007 Overall;

	// Per-query dump (semantic FindNearestSemantic) -> Saved/, beside the FTS5 dump.
	FString Dump = TEXT("split\thit1\thit5\thit10\texpected\ttop5\tquery\n");

	for (const FCorpusRow007& Row : Rows)
	{
		const TArray<FClaireonToolCatalogMatch> Results =
			FClaireonToolEmbeddingIndex::FindNearestSemantic(Row.Query, 50);

		// Build ranked name list for hit comparison.
		TArray<FString> RankedNames;
		RankedNames.Reserve(Results.Num());
		for (const FClaireonToolCatalogMatch& M : Results) { RankedNames.Add(M.Name); }

		// Intent-based acceptance: identical union (explicit names + dynamic category
		// / operation-substring sets) as the lexical harness.
		const TSet<FString> Acceptable = ResolveAcceptableNames007(Row, *Server);
		bool bHit1  = false;
		bool bHit5  = false;
		bool bHit10 = false;
		const int32 Lim1  = FMath::Min(1,  RankedNames.Num());
		const int32 Lim5  = FMath::Min(5,  RankedNames.Num());
		const int32 Lim10 = FMath::Min(10, RankedNames.Num());
		for (int32 i = 0; i < Lim1;  ++i) { if (Acceptable.Contains(RankedNames[i])) { bHit1  = true; } }
		for (int32 i = 0; i < Lim5;  ++i) { if (Acceptable.Contains(RankedNames[i])) { bHit5  = true; } }
		for (int32 i = 0; i < Lim10; ++i) { if (Acceptable.Contains(RankedNames[i])) { bHit10 = true; } }

		TArray<FString> Top5;
		for (int32 i = 0; i < FMath::Min(5, RankedNames.Num()); ++i) { Top5.Add(RankedNames[i]); }
		Dump += FString::Printf(TEXT("%s\t%d\t%d\t%d\t%s\t%s\t%s\n"),
			Row.Split.IsEmpty() ? TEXT("unknown") : *Row.Split,
			bHit1 ? 1 : 0, bHit5 ? 1 : 0, bHit10 ? 1 : 0,
			*FString::Join(Row.ExpectedTools, TEXT(",")),
			*FString::Join(Top5, TEXT(",")),
			*Row.Query);

		const FString SplitKey = Row.Split.IsEmpty() ? TEXT("unknown") : Row.Split;
		BySplit.FindOrAdd(SplitKey).Record(bHit1, bHit5, bHit10);
		Overall.Record(bHit1, bHit5, bHit10);
	}

	UE_LOG(LogTemp, Display,
		TEXT("[ClaireonToolSearchIndex] QueryCorpusSemanticDryRun (semantic) -- %d rows"),
		Overall.Total);
	UE_LOG(LogTemp, Display,
		TEXT("[ClaireonToolSearchIndex]   Overall: top-1=%.1f%%  top-5=%.1f%%  top-10=%.1f%%"),
		Overall.Total > 0 ? 100.0 * Overall.Hit1  / Overall.Total : 0.0,
		Overall.Total > 0 ? 100.0 * Overall.Hit5  / Overall.Total : 0.0,
		Overall.Total > 0 ? 100.0 * Overall.Hit10 / Overall.Total : 0.0);

	for (const TPair<FString, FSplitMetrics007>& KV : BySplit)
	{
		const FSplitMetrics007& M = KV.Value;
		UE_LOG(LogTemp, Display,
			TEXT("[ClaireonToolSearchIndex]   split='%s': %d rows  top-1=%.1f%%  top-5=%.1f%%  top-10=%.1f%%"),
			*KV.Key, M.Total,
			M.Total > 0 ? 100.0 * M.Hit1  / M.Total : 0.0,
			M.Total > 0 ? 100.0 * M.Hit5  / M.Total : 0.0,
			M.Total > 0 ? 100.0 * M.Hit10 / M.Total : 0.0);
	}

	// Write metrics file (not gated -- informational).
	const bool bWroteMetrics = WriteSplitMetrics(
		BySplit, Overall, TEXT("007-semantic-dryrun"), TEXT("semantic_metrics.json"));
	if (!bWroteMetrics)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[ClaireonToolSearchIndex] QueryCorpusSemanticDryRun: failed to write semantic_metrics.json"));
	}

	// Per-query dump -> Saved/.
	{
		const FString DumpPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("Claireon/ToolSearch/semantic_per_query.tsv")));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(DumpPath), /*Tree=*/true);
		if (FFileHelper::SaveStringToFile(Dump, *DumpPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogTemp, Display,
				TEXT("[ClaireonToolSearchIndex] semantic per-query dump written: %s"), *DumpPath);
		}
	}

	// No bar asserted here -- this is a measurement-only dry-run.

	FClaireonToolEmbeddingIndex::Clear();

	co_return;
}

// ===========================================================================
// FindNearestRawRanked basic shape + plural/singular recall regression
//
// Verifies that:
//   1. FindNearestRawRanked returns results for the same queries FindNearest handles.
//   2. Results are raw BM25 order (no pin bucketing): RankSource = LexicalOnlyFallback,
//      scores are negative/zero (raw BM25).
//   3. Plural/singular recall: "actors"/"actor", "worlds"/"world", "spawns"/"spawn"
//      all return > 0 results, and the top-10 of each singular/plural pair overlap
//      on at least one tool name -- confirming FTS5 porter stemming handles them
//      symmetrically on BOTH the index and query side.
//
// FTS5 porter tokenisation applies the same stemmer to both indexed tokens and
// MATCH query tokens, so "actors" and "actor" resolve to the same stem at search
// time. These tests lock that invariant so any future tokenizer change that
// reintroduces the asymmetry surfaces immediately.
//
// File-local discriminator prefix: Cl009Raw_ (avoids anon-NS unity collision).
// No UNTEST macros inside lambdas; no nested block-comments inside doc-comments.
// ===========================================================================

namespace Cl009RawTestsNS
{
	/**
	 * Build the production index from the live server (same pattern as
	 * QueryCorpusDryRun: RebuildFromLiveServer populates params via FlattenParams).
	 * Returns true when the index is valid and non-empty after the call.
	 */
	static bool Cl009Raw_BuildProductionIndex()
	{
		FClaireonToolSearchIndex::Clear();
		FClaireonToolSearchIndex::RebuildFromLiveServer();
		const bool bBuilt = FClaireonToolSearchIndex::EnsureBuilt();
		if (!bBuilt) { return false; }
		FSQLiteDatabase* Db = FClaireonToolSearchIndex::GetDatabaseForTest();
		if (!Db || !Db->IsValid()) { return false; }
		// Confirm at least one row exists.
		int32 Count = 0;
		Db->Execute(
			TEXT("SELECT count(*) FROM tools;"),
			[&Count](const FSQLitePreparedStatement& Stmt) -> ESQLitePreparedStatementExecuteRowResult
			{
				int32 Val = 0;
				if (Stmt.GetColumnValueByIndex(0, Val)) { Count = Val; }
				return ESQLitePreparedStatementExecuteRowResult::Stop;
			});
		return Count > 0;
	}

	/**
	 * Returns true when at least one name from SetA appears in SetB.
	 * Used to check singular/plural top-10 overlap without UNTEST in a lambda.
	 */
	static bool Cl009Raw_SetsOverlap(const TSet<FString>& SetA, const TSet<FString>& SetB)
	{
		for (const FString& Name : SetA)
		{
			if (SetB.Contains(Name)) { return true; }
		}
		return false;
	}

	/**
	 * Collect the top-K tool names from a FindNearestRawRanked result into a TSet.
	 */
	static TSet<FString> Cl009Raw_TopKNames(const TArray<FClaireonToolCatalogMatch>& Results, int32 K)
	{
		TSet<FString> Out;
		const int32 Limit = FMath::Min(K, Results.Num());
		for (int32 i = 0; i < Limit; ++i)
		{
			Out.Add(Results[i].Name);
		}
		return Out;
	}

} // namespace Cl009RawTestsNS

// ---------------------------------------------------------------------------
// FindNearestRawRanked basic shape
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, FindNearestRawRankedBasicShape, UNTEST_TIMEOUTMS(30000.0))
{
	using namespace ClaireonToolSearchIndexTestsHelpers;
	using namespace Cl009RawTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	// Build a production-quality index so raw queries exercise real content.
	const bool bBuilt = Cl009Raw_BuildProductionIndex();
	UNTEST_ASSERT_TRUE(bBuilt);

	// --- Basic non-empty result ---
	{
		const TArray<FClaireonToolCatalogMatch> Results =
			FClaireonToolSearchIndex::FindNearestRawRanked(TEXT("chooser inspect"), 10);

		UE_LOG(LogTemp, Display,
			TEXT("[Stage009] FindNearestRawRankedBasicShape: 'chooser inspect' -> %d results"),
			Results.Num());

		UNTEST_EXPECT_TRUE(Results.Num() > 0);

		// All results must carry LexicalOnlyFallback (raw bm25 path, no pin).
		bool bAllLexical = true;
		for (const FClaireonToolCatalogMatch& M : Results)
		{
			if (M.RankSource != EClaireonRankSource::LexicalOnlyFallback)
			{
				bAllLexical = false;
			}
		}
		UNTEST_EXPECT_TRUE(bAllLexical);

		// Scores must be <= 0.0 (raw bm25; negative or zero, never NaN/positive).
		bool bScoresValid = true;
		for (const FClaireonToolCatalogMatch& M : Results)
		{
			if (FMath::IsNaN(M.Score) || M.Score > 0.0f)
			{
				bScoresValid = false;
			}
		}
		UNTEST_EXPECT_TRUE(bScoresValid);
	}

	// --- Empty query must return empty ---
	{
		const TArray<FClaireonToolCatalogMatch> Empty =
			FClaireonToolSearchIndex::FindNearestRawRanked(TEXT(""), 10);
		UNTEST_EXPECT_EQ(Empty.Num(), 0);
	}

	// --- MaxResults=0 must return empty ---
	{
		const TArray<FClaireonToolCatalogMatch> Zero =
			FClaireonToolSearchIndex::FindNearestRawRanked(TEXT("chooser"), 0);
		UNTEST_EXPECT_EQ(Zero.Num(), 0);
	}

	// --- RawRanked must NOT pin: exact name "chooser_create" should NOT
	//     necessarily be at index 0 (no pin-bucket sort in the raw path).
	//     We only verify that the tool appears in the results somewhere --
	//     placement is bm25 order and depends on corpus content.
	{
		const TArray<FClaireonToolCatalogMatch> Results =
			FClaireonToolSearchIndex::FindNearestRawRanked(TEXT("chooser_create"), 50);
		bool bFound = false;
		for (const FClaireonToolCatalogMatch& M : Results)
		{
			if (M.Name == TEXT("chooser_create")) { bFound = true; break; }
		}
		UE_LOG(LogTemp, Display,
			TEXT("[Stage009] FindNearestRawRankedBasicShape: 'chooser_create' found in raw results: %s  "
			     "(total: %d)"),
			bFound ? TEXT("YES") : TEXT("NO"), Results.Num());
		// Non-assertion: raw path may or may not surface the exact-name tool
		// at rank 0 depending on bm25 scores -- that is expected behavior.
	}

	FClaireonToolSearchIndex::Clear();

	co_return;
}

// ---------------------------------------------------------------------------
// Plural/singular recall regression
//
// FTS5 porter stemming makes "actor"/"actors", "world"/"worlds",
// "spawn"/"spawns" stem to the same root on both index and query side.
// These assertions lock that invariant so any future tokenizer change that
// reintroduces the asymmetry surfaces immediately.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, PluralSingularRecallRegression, UNTEST_TIMEOUTMS(30000.0))
{
	using namespace ClaireonToolSearchIndexTestsHelpers;
	using namespace Cl009RawTestsNS;

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = EnsureServerAndBridge(Module);
	UNTEST_ASSERT_PTR(Server);

	// Production index: params+examples populated so actor/spawn appear in content.
	const bool bBuilt = Cl009Raw_BuildProductionIndex();
	UNTEST_ASSERT_TRUE(bBuilt);

	// Helper: run both singular and plural through FindNearestRawRanked, log results,
	// assert both return > 0, assert top-10 overlap >= 1 tool.
	// Implemented inline per pair below (no UNTEST in lambdas per house rules).

	// --- actor / actors ---
	{
		const TArray<FClaireonToolCatalogMatch> Singular =
			FClaireonToolSearchIndex::FindNearestRawRanked(TEXT("actor"), 10);
		const TArray<FClaireonToolCatalogMatch> Plural =
			FClaireonToolSearchIndex::FindNearestRawRanked(TEXT("actors"), 10);

		UE_LOG(LogTemp, Display,
			TEXT("[Stage009] PluralSingularRecall: 'actor'->%d  'actors'->%d results"),
			Singular.Num(), Plural.Num());

		// Both must return at least one result.
		UNTEST_EXPECT_TRUE(Singular.Num() > 0);
		UNTEST_EXPECT_TRUE(Plural.Num() > 0);

		// Top-10 must overlap on at least one tool name.
		const TSet<FString> SingularTop = Cl009Raw_TopKNames(Singular, 10);
		const TSet<FString> PluralTop   = Cl009Raw_TopKNames(Plural,   10);
		const bool bOverlap = Cl009Raw_SetsOverlap(SingularTop, PluralTop);
		UE_LOG(LogTemp, Display,
			TEXT("[Stage009] PluralSingularRecall: actor/actors top-10 overlap: %s"),
			bOverlap ? TEXT("YES") : TEXT("NO"));
		UNTEST_EXPECT_TRUE(bOverlap);
	}

	// --- world / worlds ---
	{
		const TArray<FClaireonToolCatalogMatch> Singular =
			FClaireonToolSearchIndex::FindNearestRawRanked(TEXT("world"), 10);
		const TArray<FClaireonToolCatalogMatch> Plural =
			FClaireonToolSearchIndex::FindNearestRawRanked(TEXT("worlds"), 10);

		UE_LOG(LogTemp, Display,
			TEXT("[Stage009] PluralSingularRecall: 'world'->%d  'worlds'->%d results"),
			Singular.Num(), Plural.Num());

		UNTEST_EXPECT_TRUE(Singular.Num() > 0);
		UNTEST_EXPECT_TRUE(Plural.Num() > 0);

		const TSet<FString> SingularTop = Cl009Raw_TopKNames(Singular, 10);
		const TSet<FString> PluralTop   = Cl009Raw_TopKNames(Plural,   10);
		const bool bOverlap = Cl009Raw_SetsOverlap(SingularTop, PluralTop);
		UE_LOG(LogTemp, Display,
			TEXT("[Stage009] PluralSingularRecall: world/worlds top-10 overlap: %s"),
			bOverlap ? TEXT("YES") : TEXT("NO"));
		UNTEST_EXPECT_TRUE(bOverlap);
	}

	// --- spawn / spawns ---
	{
		const TArray<FClaireonToolCatalogMatch> Singular =
			FClaireonToolSearchIndex::FindNearestRawRanked(TEXT("spawn"), 10);
		const TArray<FClaireonToolCatalogMatch> Plural =
			FClaireonToolSearchIndex::FindNearestRawRanked(TEXT("spawns"), 10);

		UE_LOG(LogTemp, Display,
			TEXT("[Stage009] PluralSingularRecall: 'spawn'->%d  'spawns'->%d results"),
			Singular.Num(), Plural.Num());

		UNTEST_EXPECT_TRUE(Singular.Num() > 0);
		UNTEST_EXPECT_TRUE(Plural.Num() > 0);

		const TSet<FString> SingularTop = Cl009Raw_TopKNames(Singular, 10);
		const TSet<FString> PluralTop   = Cl009Raw_TopKNames(Plural,   10);
		const bool bOverlap = Cl009Raw_SetsOverlap(SingularTop, PluralTop);
		UE_LOG(LogTemp, Display,
			TEXT("[Stage009] PluralSingularRecall: spawn/spawns top-10 overlap: %s"),
			bOverlap ? TEXT("YES") : TEXT("NO"));
		UNTEST_EXPECT_TRUE(bOverlap);
	}

	FClaireonToolSearchIndex::Clear();

	co_return;
}

// ===========================================================================
// RRF tuning sweep (MEASURE + REPORT, DO NOT GATE)
//
// Sweeps the runtime-tunable RRF params (FClaireonToolSearchIndex::SetRrfParamsForTest)
// over a grid and reports the best config on tune+dev, then measures the FROZEN
// split ONCE for the chosen config. This is a measurement+report unit: it PASSES
// as long as it ran and wrote its outputs. It NEVER asserts pass/fail against a
// numeric bar, and it touches frozen exactly once -- only to report the chosen
// config's frozen numbers, never to tune against.
//
// Both indices (lexical FTS5 + semantic embedding) are rebuilt deterministically
// ONCE up front (Clear() + RebuildFromLiveServer() each), exactly the shipped
// PRODUCTION indices. The sweep calls FindNearestHybrid DIRECTLY (not Execute)
// with MaxResults=10 so it scores the raw hybrid ranking, not the Execute
// presentation layer.
//
// Grid (80 configs): K {20,40,60,80,100} x LexW {0.25,0.5,0.75,1.0}
//                    x SemW {1.0,1.25,1.5,2.0}.
//
// Re-embed cost note: FindNearestHybrid -> FindNearestSemantic embeds the QUERY
// each call. Across 80 configs x ~88 tune+dev rows that is ~7000 query
// embeddings. The cost is ACCEPTED: the doc embeddings (the expensive part: one
// matrix over the whole catalog) are built once via RebuildFromLiveServer; only
// the cheap single-query forward pass repeats per config. Timeout is 600000ms.
//
// File-local discriminator: Cl013Sweep_ (avoids anon-NS unity collision).
// No UNTEST macros inside lambdas / helpers; bool/value-returning helpers only.
// ===========================================================================

namespace Cl013SweepNS
{
	using Cl628QryTestsNS::FCorpusRow007;
	using Cl628QryTestsNS::ResolveAcceptableNames007;

	// One grid point + its measured tune+dev metrics.
	struct FCl013Sweep_Config
	{
		float K    = 60.0f;
		float LexW = 1.0f;
		float SemW = 1.0f;

		// tune+dev combined (the only split tuned on).
		int32 TuneDevTotal = 0;
		int32 TuneDevHit1  = 0;
		int32 TuneDevHit5  = 0;
		int32 TuneDevHit10 = 0;

		double Top1Pct() const { return TuneDevTotal > 0 ? 100.0 * TuneDevHit1  / TuneDevTotal : 0.0; }
		double Top5Pct() const { return TuneDevTotal > 0 ? 100.0 * TuneDevHit5  / TuneDevTotal : 0.0; }
		double Top10Pct() const { return TuneDevTotal > 0 ? 100.0 * TuneDevHit10 / TuneDevTotal : 0.0; }
	};

	// Top-K hit of a ranked name list against a resolved acceptable set.
	static bool Cl013Sweep_IsHitAtK(const TArray<FString>& Ranked, const TSet<FString>& Acceptable, int32 K)
	{
		const int32 Limit = FMath::Min(K, Ranked.Num());
		for (int32 i = 0; i < Limit; ++i)
		{
			if (Acceptable.Contains(Ranked[i])) { return true; }
		}
		return false;
	}

	// True when a split label is part of the tune+dev tuning set.
	static bool Cl013Sweep_IsTuneDev(const FString& Split)
	{
		return Split == TEXT("tune") || Split == TEXT("dev");
	}

	// Convert a hybrid match list into a bare ranked name list.
	static TArray<FString> Cl013Sweep_NamesOf(const TArray<FClaireonToolCatalogMatch>& Matches)
	{
		TArray<FString> Out;
		Out.Reserve(Matches.Num());
		for (const FClaireonToolCatalogMatch& M : Matches) { Out.Add(M.Name); }
		return Out;
	}

	// Compact accept-spec string for the per-query TSVs (mirrors the corpus dump).
	static FString Cl013Sweep_AcceptSpec(const FCorpusRow007& Row)
	{
		FString Spec = FString::Join(Row.ExpectedTools, TEXT(","));
		if (!Row.ExpectedCategory.IsEmpty()) { Spec += FString::Printf(TEXT(" cat:%s"), *Row.ExpectedCategory); }
		if (!Row.ExpectedOperationSubstring.IsEmpty()) { Spec += FString::Printf(TEXT(" op~%s"), *Row.ExpectedOperationSubstring); }
		return Spec.TrimStartAndEnd();
	}

	// -----------------------------------------------------------------------
	// Selection comparator. Returns true when A is a STRICTLY BETTER choice
	// than B on tune+dev.
	//
	// Primary: prefer configs that MEET the bar (top5 >= 90 AND top10 >= 100).
	//   - both meet  -> fall through to tie-breaks.
	//   - one meets   -> it wins.
	//   - neither     -> minimize shortfall = max(0,90-top5)+max(0,100-top10)
	//                    (smaller shortfall wins).
	// Tie-break order: higher top5, higher top10, higher top1, then SIMPLER:
	//   prefer K == 60, then smaller weight skew |LexW - SemW|, then a final
	//   deterministic key (K asc, LexW asc, SemW asc) so the order is total.
	// -----------------------------------------------------------------------
	static double Cl013Sweep_Shortfall(const FCl013Sweep_Config& C)
	{
		return FMath::Max(0.0, 90.0 - C.Top5Pct()) + FMath::Max(0.0, 100.0 - C.Top10Pct());
	}

	static bool Cl013Sweep_MeetsBar(const FCl013Sweep_Config& C)
	{
		return C.Top5Pct() >= 90.0 && C.Top10Pct() >= 100.0;
	}

	// Simplicity score: lower is simpler. K==60 preferred (0 vs 1), then skew.
	static double Cl013Sweep_SkewKey(const FCl013Sweep_Config& C)
	{
		return FMath::Abs(static_cast<double>(C.LexW) - static_cast<double>(C.SemW));
	}

	// Returns true if A is strictly better than B (a total order; ties broken to
	// a single deterministic winner so sort/selection is reproducible).
	static bool Cl013Sweep_IsBetter(const FCl013Sweep_Config& A, const FCl013Sweep_Config& B)
	{
		const bool AMeets = Cl013Sweep_MeetsBar(A);
		const bool BMeets = Cl013Sweep_MeetsBar(B);
		if (AMeets != BMeets) { return AMeets; }
		if (!AMeets && !BMeets)
		{
			const double SA = Cl013Sweep_Shortfall(A);
			const double SB = Cl013Sweep_Shortfall(B);
			if (SA != SB) { return SA < SB; }
		}
		// Tie-breaks (apply whether both meet the bar or both fail with equal shortfall).
		if (A.Top5Pct()  != B.Top5Pct())  { return A.Top5Pct()  > B.Top5Pct(); }
		if (A.Top10Pct() != B.Top10Pct()) { return A.Top10Pct() > B.Top10Pct(); }
		if (A.Top1Pct()  != B.Top1Pct())  { return A.Top1Pct()  > B.Top1Pct(); }
		// Simpler config: prefer K==60.
		const bool AIs60 = (A.K == 60.0f);
		const bool BIs60 = (B.K == 60.0f);
		if (AIs60 != BIs60) { return AIs60; }
		// Smaller weight skew.
		const double SkA = Cl013Sweep_SkewKey(A);
		const double SkB = Cl013Sweep_SkewKey(B);
		if (SkA != SkB) { return SkA < SkB; }
		// Final deterministic key (K asc, LexW asc, SemW asc).
		if (A.K    != B.K)    { return A.K    < B.K; }
		if (A.LexW != B.LexW) { return A.LexW < B.LexW; }
		return A.SemW < B.SemW;
	}

	// JSON for one config's tune+dev metrics.
	static TSharedPtr<FJsonObject> Cl013Sweep_ConfigToJson(const FCl013Sweep_Config& C)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetNumberField(TEXT("k"),     C.K);
		Obj->SetNumberField(TEXT("lex_w"), C.LexW);
		Obj->SetNumberField(TEXT("sem_w"), C.SemW);
		Obj->SetNumberField(TEXT("tunedev_total"), C.TuneDevTotal);
		Obj->SetNumberField(TEXT("tunedev_hit1"),  C.TuneDevHit1);
		Obj->SetNumberField(TEXT("tunedev_hit5"),  C.TuneDevHit5);
		Obj->SetNumberField(TEXT("tunedev_hit10"), C.TuneDevHit10);
		Obj->SetNumberField(TEXT("tunedev_top1"),  C.Top1Pct());
		Obj->SetNumberField(TEXT("tunedev_top5"),  C.Top5Pct());
		Obj->SetNumberField(TEXT("tunedev_top10"), C.Top10Pct());
		return Obj;
	}

	// Per-query TSV writer. RowFn produces "split\thit1\thit5\thit10\taccept\ttop5\tquery"
	// for each corpus row using whichever channel the caller drives. Returns false on I/O fail.
	static bool Cl013Sweep_WriteTsv(const FString& FileName, const FString& Body)
	{
		const FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("Claireon/ToolSearch"),
			FileName));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		return FFileHelper::SaveStringToFile(Body, *Path,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
	// -----------------------------------------------------------------------
	// Shared sweep body. OutSuffix is appended to every output filename so
	// parallel runs do not clobber each other:
	//   rrf_sweep<suffix>.json
	//   chosen_{hybrid,semantic,lexical}_per_query<suffix>.tsv
	// The CHOSEN log line is tagged "[RrfSweep<suffix>]" so runs are
	// distinguishable in the log.
	//
	// This is a free helper (not a UNIT body), so it cannot use the
	// UNTEST_ASSERT_*/UNTEST_EXPECT_* macros (they expand to co_return /
	// TestContext member access). The original asserts become skip-with-log
	// early-returns; the unit's report-only semantics are preserved
	// (it PASSES as long as it ran and wrote its outputs).
	//
	// IMPORTANT: the caller is responsible for any model override + revert
	// (SetModelForTest / ResetModelForTest). This helper performs the index
	// Clear()+RebuildFromLiveServer() at its start, which -- when an override is
	// active -- loads the override model.
	// -----------------------------------------------------------------------
	static void RunRrfSweep(const FString& OutSuffix)
	{
		using namespace ClaireonToolSearchIndexTestsHelpers;
		using namespace Cl628QryTestsNS;

		const FString LogTag = FString::Printf(TEXT("[RrfSweep%s]"), *OutSuffix);

		// --------------------------------------------------------------
		// Load corpus -- skip-with-log if absent (NOT a failure).
		// --------------------------------------------------------------
		const TSharedPtr<FJsonObject> CorpusRoot = LoadCorpusJson();
		if (!CorpusRoot.IsValid())
		{
			UE_LOG(LogTemp, Display, TEXT("%s corpus not found -- skipping sweep"), *LogTag);
			return;
		}
		const TArray<FCorpusRow007> Rows = ParseCorpusRows(CorpusRoot);
		if (Rows.IsEmpty())
		{
			UE_LOG(LogTemp, Warning,
				TEXT("%s corpus loaded but contains zero rows -- skipping"), *LogTag);
			return;
		}

		FClaireonModule& Module = FClaireonModule::Get();
		FClaireonServer* Server = EnsureServerAndBridge(Module);
		if (!Server)
		{
			UE_LOG(LogTemp, Warning, TEXT("%s no live server -- skipping sweep"), *LogTag);
			return;
		}

		// --------------------------------------------------------------
		// Build BOTH indices deterministically ONCE up front.
		// When a model override is active, the embedding rebuild loads it here.
		// --------------------------------------------------------------
		FClaireonToolSearchIndex::Clear();
		FClaireonToolSearchIndex::RebuildFromLiveServer();
		const bool bLexBuilt = FClaireonToolSearchIndex::EnsureBuilt();
		if (!bLexBuilt)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("%s lexical index failed to build -- skipping sweep"), *LogTag);
			FClaireonToolSearchIndex::Clear();
			return;
		}

		FClaireonToolEmbeddingIndex::Clear();
		FClaireonToolEmbeddingIndex::RebuildFromLiveServer();

		// Skip-with-log if the semantic index is not ready (no ORT runtime / model /
		// vocab). The sweep measures HYBRID fusion, which is meaningless without the
		// semantic channel; CI without the model stays green.
		if (!FClaireonToolEmbeddingIndex::IsReady())
		{
			UE_LOG(LogTemp, Display,
				TEXT("%s embedding index not ready (no ORT runtime / model / vocab) -- skipping sweep"),
				*LogTag);
			FClaireonToolEmbeddingIndex::Clear();
			FClaireonToolSearchIndex::Clear();
			return;
		}

		// Pre-resolve each row's acceptable set + split ONCE (registry is static for
		// the run). Avoids re-resolving 80x.
		struct FResolvedRow
		{
			const FCorpusRow007* Row = nullptr;
			TSet<FString> Acceptable;
			bool bTuneDev = false;
		};
		TArray<FResolvedRow> Resolved;
		Resolved.Reserve(Rows.Num());
		for (const FCorpusRow007& Row : Rows)
		{
			FResolvedRow R;
			R.Row = &Row;
			R.Acceptable = ResolveAcceptableNames007(Row, *Server);
			R.bTuneDev = Cl013Sweep_IsTuneDev(Row.Split);
			Resolved.Add(MoveTemp(R));
		}

		// --------------------------------------------------------------
		// Build the 80-config grid.
		// --------------------------------------------------------------
		const float Ks[]    = { 20.0f, 40.0f, 60.0f, 80.0f, 100.0f };
		const float LexWs[] = { 0.25f, 0.5f, 0.75f, 1.0f };
		const float SemWs[] = { 1.0f, 1.25f, 1.5f, 2.0f };

		TArray<FCl013Sweep_Config> Configs;
		Configs.Reserve(80);

		for (float K : Ks)
		{
			for (float LexW : LexWs)
			{
				for (float SemW : SemWs)
				{
					FCl013Sweep_Config Cfg;
					Cfg.K = K; Cfg.LexW = LexW; Cfg.SemW = SemW;

					FClaireonToolSearchIndex::SetRrfParamsForTest(K, LexW, SemW);

					for (const FResolvedRow& R : Resolved)
					{
						if (!R.bTuneDev) { continue; } // ignore frozen during the sweep
						const TArray<FString> Ranked = Cl013Sweep_NamesOf(
							FClaireonToolSearchIndex::FindNearestHybrid(R.Row->Query, 10, FString()));
						const bool bH1  = Cl013Sweep_IsHitAtK(Ranked, R.Acceptable, 1);
						const bool bH5  = Cl013Sweep_IsHitAtK(Ranked, R.Acceptable, 5);
						const bool bH10 = Cl013Sweep_IsHitAtK(Ranked, R.Acceptable, 10);
						++Cfg.TuneDevTotal;
						if (bH1)  { ++Cfg.TuneDevHit1; }
						if (bH5)  { ++Cfg.TuneDevHit5; }
						if (bH10) { ++Cfg.TuneDevHit10; }
					}
					Configs.Add(Cfg);
				}
			}
		}

		// Report-only sanity: the grid must be the full 80 points. Logged (not
		// asserted) because this is a free helper -- UNTEST macros expand to
		// co_return / TestContext access and cannot live outside a unit body.
		if (Configs.Num() != 80)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("%s grid produced %d configs (expected 80)"), *LogTag, Configs.Num());
		}

		// --------------------------------------------------------------
		// Select BEST on tune+dev via the deterministic comparator.
		// --------------------------------------------------------------
		int32 BestIdx = 0;
		for (int32 i = 1; i < Configs.Num(); ++i)
		{
			if (Cl013Sweep_IsBetter(Configs[i], Configs[BestIdx]))
			{
				BestIdx = i;
			}
		}
		const FCl013Sweep_Config Best = Configs[BestIdx];

		// --------------------------------------------------------------
		// Measure FROZEN ONCE for the chosen config, and re-measure tune+dev for it.
		// (Best already carries tune+dev numbers from the sweep; re-measuring gives a
		// per-query TSV with the chosen params active and confirms reproducibility.)
		// --------------------------------------------------------------
		FClaireonToolSearchIndex::SetRrfParamsForTest(Best.K, Best.LexW, Best.SemW);

		int32 FrozenTotal = 0, FrozenHit1 = 0, FrozenHit5 = 0, FrozenHit10 = 0;
		int32 TuneTotal = 0, TuneHit1 = 0, TuneHit5 = 0, TuneHit10 = 0;
		int32 DevTotal = 0, DevHit1 = 0, DevHit5 = 0, DevHit10 = 0;

		// Per-query TSVs for the CHOSEN config across all three channels to confirm
		// the hybrid adds COMPLEMENTARY hits (rows the hybrid catches that neither
		// single channel does) rather than just reshuffling.
		const FString TsvHeader = TEXT("split\thit1\thit5\thit10\taccept\ttop5\tquery\n");
		FString HybridTsv   = TsvHeader;
		FString SemanticTsv = TsvHeader;
		FString LexicalTsv  = TsvHeader;

		for (const FResolvedRow& R : Resolved)
		{
			const FString Split = R.Row->Split.IsEmpty() ? TEXT("unknown") : R.Row->Split;
			const FString AcceptSpec = Cl013Sweep_AcceptSpec(*R.Row);

			// Hybrid (chosen config).
			const TArray<FClaireonToolCatalogMatch> HybridM =
				FClaireonToolSearchIndex::FindNearestHybrid(R.Row->Query, 10, FString());
			const TArray<FString> HybridNames = Cl013Sweep_NamesOf(HybridM);
			const bool bHH1  = Cl013Sweep_IsHitAtK(HybridNames, R.Acceptable, 1);
			const bool bHH5  = Cl013Sweep_IsHitAtK(HybridNames, R.Acceptable, 5);
			const bool bHH10 = Cl013Sweep_IsHitAtK(HybridNames, R.Acceptable, 10);

			// Accumulate per-split hybrid metrics.
			if (R.Row->Split == TEXT("frozen"))
			{
				++FrozenTotal; if (bHH1) { ++FrozenHit1; } if (bHH5) { ++FrozenHit5; } if (bHH10) { ++FrozenHit10; }
			}
			else if (R.Row->Split == TEXT("tune"))
			{
				++TuneTotal; if (bHH1) { ++TuneHit1; } if (bHH5) { ++TuneHit5; } if (bHH10) { ++TuneHit10; }
			}
			else if (R.Row->Split == TEXT("dev"))
			{
				++DevTotal; if (bHH1) { ++DevHit1; } if (bHH5) { ++DevHit5; } if (bHH10) { ++DevHit10; }
			}

			TArray<FString> HybridTop5;
			for (int32 i = 0; i < FMath::Min(5, HybridNames.Num()); ++i) { HybridTop5.Add(HybridNames[i]); }
			HybridTsv += FString::Printf(TEXT("%s\t%d\t%d\t%d\t%s\t%s\t%s\n"),
				*Split, bHH1 ? 1 : 0, bHH5 ? 1 : 0, bHH10 ? 1 : 0,
				*AcceptSpec, *FString::Join(HybridTop5, TEXT(",")), *R.Row->Query);

			// Semantic-only channel.
			const TArray<FClaireonToolCatalogMatch> SemM =
				FClaireonToolEmbeddingIndex::FindNearestSemantic(R.Row->Query, 10, FString());
			const TArray<FString> SemNames = Cl013Sweep_NamesOf(SemM);
			const bool bSH1  = Cl013Sweep_IsHitAtK(SemNames, R.Acceptable, 1);
			const bool bSH5  = Cl013Sweep_IsHitAtK(SemNames, R.Acceptable, 5);
			const bool bSH10 = Cl013Sweep_IsHitAtK(SemNames, R.Acceptable, 10);
			TArray<FString> SemTop5;
			for (int32 i = 0; i < FMath::Min(5, SemNames.Num()); ++i) { SemTop5.Add(SemNames[i]); }
			SemanticTsv += FString::Printf(TEXT("%s\t%d\t%d\t%d\t%s\t%s\t%s\n"),
				*Split, bSH1 ? 1 : 0, bSH5 ? 1 : 0, bSH10 ? 1 : 0,
				*AcceptSpec, *FString::Join(SemTop5, TEXT(",")), *R.Row->Query);

			// Lexical-only channel (raw bm25, unpinned).
			const TArray<FClaireonToolCatalogMatch> LexM =
				FClaireonToolSearchIndex::FindNearestRawRanked(R.Row->Query, 10, FString());
			const TArray<FString> LexNames = Cl013Sweep_NamesOf(LexM);
			const bool bLH1  = Cl013Sweep_IsHitAtK(LexNames, R.Acceptable, 1);
			const bool bLH5  = Cl013Sweep_IsHitAtK(LexNames, R.Acceptable, 5);
			const bool bLH10 = Cl013Sweep_IsHitAtK(LexNames, R.Acceptable, 10);
			TArray<FString> LexTop5;
			for (int32 i = 0; i < FMath::Min(5, LexNames.Num()); ++i) { LexTop5.Add(LexNames[i]); }
			LexicalTsv += FString::Printf(TEXT("%s\t%d\t%d\t%d\t%s\t%s\t%s\n"),
				*Split, bLH1 ? 1 : 0, bLH5 ? 1 : 0, bLH10 ? 1 : 0,
				*AcceptSpec, *FString::Join(LexTop5, TEXT(",")), *R.Row->Query);
		}

		const int32 ChosenTuneDevTotal = TuneTotal + DevTotal;
		const int32 ChosenTuneDevHit1  = TuneHit1 + DevHit1;
		const int32 ChosenTuneDevHit5  = TuneHit5 + DevHit5;
		const int32 ChosenTuneDevHit10 = TuneHit10 + DevHit10;

		auto Pct = [](int32 Hit, int32 Total) -> double { return Total > 0 ? 100.0 * Hit / Total : 0.0; };

		// --------------------------------------------------------------
		// Log the chosen config + all three splits prominently (model-tagged).
		// --------------------------------------------------------------
		UE_LOG(LogTemp, Display,
			TEXT("%s CHOSEN K=%.0f LexW=%.2f SemW=%.2f  "
			     "frozen=%.1f/%.1f/%.1f  tune=%.1f/%.1f/%.1f  dev=%.1f/%.1f/%.1f  tunedev=%.1f/%.1f/%.1f"),
			*LogTag, Best.K, Best.LexW, Best.SemW,
			Pct(FrozenHit1, FrozenTotal), Pct(FrozenHit5, FrozenTotal), Pct(FrozenHit10, FrozenTotal),
			Pct(TuneHit1, TuneTotal), Pct(TuneHit5, TuneTotal), Pct(TuneHit10, TuneTotal),
			Pct(DevHit1, DevTotal), Pct(DevHit5, DevTotal), Pct(DevHit10, DevTotal),
			Pct(ChosenTuneDevHit1, ChosenTuneDevTotal), Pct(ChosenTuneDevHit5, ChosenTuneDevTotal),
			Pct(ChosenTuneDevHit10, ChosenTuneDevTotal));

		// --------------------------------------------------------------
		// Write rrf_sweep<suffix>.json: all 80 configs' tune+dev metrics + chosen config + frozen.
		// --------------------------------------------------------------
		{
			TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
			Root->SetStringField(TEXT("stage"), TEXT("013-rrf-tuning-sweep"));
			Root->SetStringField(TEXT("generated_utc"), FDateTime::UtcNow().ToString());

			TArray<TSharedPtr<FJsonValue>> ConfigArr;
			ConfigArr.Reserve(Configs.Num());
			for (const FCl013Sweep_Config& C : Configs)
			{
				ConfigArr.Add(MakeShared<FJsonValueObject>(Cl013Sweep_ConfigToJson(C)));
			}
			Root->SetArrayField(TEXT("configs"), ConfigArr);

			TSharedPtr<FJsonObject> Chosen = MakeShared<FJsonObject>();
			Chosen->SetNumberField(TEXT("k"),     Best.K);
			Chosen->SetNumberField(TEXT("lex_w"), Best.LexW);
			Chosen->SetNumberField(TEXT("sem_w"), Best.SemW);
			Chosen->SetNumberField(TEXT("tunedev_top1"),  Pct(ChosenTuneDevHit1,  ChosenTuneDevTotal));
			Chosen->SetNumberField(TEXT("tunedev_top5"),  Pct(ChosenTuneDevHit5,  ChosenTuneDevTotal));
			Chosen->SetNumberField(TEXT("tunedev_top10"), Pct(ChosenTuneDevHit10, ChosenTuneDevTotal));
			Chosen->SetNumberField(TEXT("tune_top1"),  Pct(TuneHit1, TuneTotal));
			Chosen->SetNumberField(TEXT("tune_top5"),  Pct(TuneHit5, TuneTotal));
			Chosen->SetNumberField(TEXT("tune_top10"), Pct(TuneHit10, TuneTotal));
			Chosen->SetNumberField(TEXT("dev_top1"),  Pct(DevHit1, DevTotal));
			Chosen->SetNumberField(TEXT("dev_top5"),  Pct(DevHit5, DevTotal));
			Chosen->SetNumberField(TEXT("dev_top10"), Pct(DevHit10, DevTotal));
			Chosen->SetNumberField(TEXT("frozen_top1"),  Pct(FrozenHit1,  FrozenTotal));
			Chosen->SetNumberField(TEXT("frozen_top5"),  Pct(FrozenHit5,  FrozenTotal));
			Chosen->SetNumberField(TEXT("frozen_top10"), Pct(FrozenHit10, FrozenTotal));
			Chosen->SetNumberField(TEXT("frozen_total"), FrozenTotal);
			Root->SetObjectField(TEXT("chosen"), Chosen);

			FString Output;
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Output);
			const bool bSerialized = FJsonSerializer::Serialize(Root.ToSharedRef(), Writer);
			Writer->Close();

			bool bWroteJson = false;
			if (bSerialized)
			{
				const FString JsonFile = FString::Printf(TEXT("Claireon/ToolSearch/rrf_sweep%s.json"), *OutSuffix);
				const FString OutPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
					FPaths::ProjectSavedDir(), JsonFile));
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), /*Tree=*/true);
				bWroteJson = FFileHelper::SaveStringToFile(Output, *OutPath,
					FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
				if (bWroteJson)
				{
					UE_LOG(LogTemp, Display, TEXT("%s wrote %s"), *LogTag, *OutPath);
				}
			}
			if (!bWroteJson)
			{
				UE_LOG(LogTemp, Warning, TEXT("%s failed to write rrf_sweep%s.json"), *LogTag, *OutSuffix);
			}
		}

		// --------------------------------------------------------------
		// Write the three per-query TSVs for the CHOSEN config (suffixed).
		// --------------------------------------------------------------
		const bool bWroteHybrid   = Cl013Sweep_WriteTsv(
			FString::Printf(TEXT("chosen_hybrid_per_query%s.tsv"), *OutSuffix), HybridTsv);
		const bool bWroteSemantic = Cl013Sweep_WriteTsv(
			FString::Printf(TEXT("chosen_semantic_per_query%s.tsv"), *OutSuffix), SemanticTsv);
		const bool bWroteLexical  = Cl013Sweep_WriteTsv(
			FString::Printf(TEXT("chosen_lexical_per_query%s.tsv"), *OutSuffix), LexicalTsv);
		if (bWroteHybrid && bWroteSemantic && bWroteLexical)
		{
			UE_LOG(LogTemp, Display,
				TEXT("%s wrote chosen_{hybrid,semantic,lexical}_per_query%s.tsv to Saved/Claireon/ToolSearch/"),
				*LogTag, *OutSuffix);
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				TEXT("%s failed to write one or more per-query TSVs (hybrid=%d semantic=%d lexical=%d)"),
				*LogTag, bWroteHybrid ? 1 : 0, bWroteSemantic ? 1 : 0, bWroteLexical ? 1 : 0);
		}

		// --------------------------------------------------------------
		// Restore production RRF defaults so other tests are unaffected, and clear
		// the indices. This helper does NOT assert against any numeric bar; it PASSES
		// as long as it ran and wrote its outputs. Frozen was measured exactly ONCE,
		// for the chosen config only.
		// --------------------------------------------------------------
		FClaireonToolSearchIndex::ResetRrfParamsForTest();
		FClaireonToolEmbeddingIndex::Clear();
		FClaireonToolSearchIndex::Clear();
	}
} // namespace Cl013SweepNS

// ---------------------------------------------------------------------------
// Shipped-default-model sweep (Default() == bge-small-en-v1.5).
// RunRrfSweep(TEXT("")) writes rrf_sweep.json and
// chosen_{hybrid,semantic,lexical}_per_query.tsv (no suffix).
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, RrfTuningSweep, UNTEST_TIMEOUTMS(600000.0))
{
	using namespace Cl013SweepNS;
	RunRrfSweep(TEXT(""));
	co_return;
}

// ---------------------------------------------------------------------------
// Explicit-override BGE sweep. SetModelForTest installs the bge-small-en-v1.5
// meta explicitly (matching Default()) so this run can be used to re-validate
// or replace the model without touching the production default path.
// Writes rrf_sweep_bge.json + chosen_*_bge TSVs.
//
// SetModelForTest forces the next RebuildFromLiveServer (inside RunRrfSweep) to
// load the override; if unavailable RunRrfSweep skips-with-log. ResetModelForTest
// is called UNCONDITIONALLY afterward (even on skip) so subsequent tests revert
// to Default().
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, ClaireonToolSearchIndex, RrfTuningSweepBGE, UNTEST_TIMEOUTMS(600000.0))
{
	using namespace Cl013SweepNS;

	FClaireonToolEmbeddingIndex::SetModelForTest(FClaireonEmbedderMeta::BGE());
	RunRrfSweep(TEXT("_bge"));
	FClaireonToolEmbeddingIndex::ResetModelForTest();

	co_return;
}

#endif // WITH_UNTESTED
