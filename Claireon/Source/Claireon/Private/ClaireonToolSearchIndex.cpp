// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonToolSearchIndex.h"

#include "ClaireonToolCatalogAbbreviations.h"
#include "ClaireonToolEmbeddingIndex.h"   // FindNearestSemantic / IsReady (RRF semantic channel)
#include "ClaireonLog.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Tools/IClaireonTool.h"
#include "SQLiteDatabase.h"
#include "SQLitePreparedStatement.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Static storage
// ---------------------------------------------------------------------------

static TUniquePtr<FSQLiteDatabase> GToolSearchDb;
static FCriticalSection GToolSearchLock;

// ---------------------------------------------------------------------------
// FTS5 schema
//   - Indexed full-text columns: name, keywords, category_operation, params,
//     description, examples
//   - UNINDEXED metadata columns: category, operation  (for filtering/output
//     without affecting bm25 scores)
//   - tokenize='porter unicode61' for stemming + Unicode case folding
// ---------------------------------------------------------------------------

static const TCHAR* GCreateToolsTable =
	TEXT("CREATE VIRTUAL TABLE IF NOT EXISTS tools USING fts5("
	     "name, keywords, category_operation, params, description, examples, "
	     "category UNINDEXED, operation UNINDEXED, "
	     "tokenize='porter unicode61');");

// ---------------------------------------------------------------------------
// Abbreviation enrichment helpers
//
// Index-side enrich: take raw field text and return it with synonym expansions
// and reverse abbreviations appended. This ensures FTS5 indexes both the
// original tokens and their abbreviation counterparts.
//
// File-local discriminator prefix (Cl628Idx_) to avoid anon-NS collisions
// under unity batching.
// ---------------------------------------------------------------------------

namespace Cl628IdxInternal
{
	static const TMap<FString, TArray<FString>>& GetForwardMap()
	{
		static const TMap<FString, TArray<FString>> Map = []()
		{
			TMap<FString, TArray<FString>> Out;
			for (const ClaireonToolCatalogAbbreviations::FEntry& E : ClaireonToolCatalogAbbreviations::GetTable())
			{
				TArray<FString> Parts;
				FString(E.Expansion).ParseIntoArray(Parts, TEXT(" "), /*CullEmpty=*/true);
				for (FString& P : Parts) { P.ToLowerInline(); }
				Out.Add(FString(E.Key).ToLower(), MoveTemp(Parts));
			}
			return Out;
		}();
		return Map;
	}

	static const TMap<FString, TArray<FString>>& GetReverseMap()
	{
		static const TMap<FString, TArray<FString>> Map = []()
		{
			TMap<FString, TArray<FString>> Out;
			for (const ClaireonToolCatalogAbbreviations::FEntry& E : ClaireonToolCatalogAbbreviations::GetTable())
			{
				const FString Key = FString(E.Key).ToLower();
				TArray<FString> Parts;
				FString(E.Expansion).ParseIntoArray(Parts, TEXT(" "), /*CullEmpty=*/true);
				for (FString& P : Parts)
				{
					P.ToLowerInline();
					Out.FindOrAdd(P).AddUnique(Key);
				}
			}
			return Out;
		}();
		return Map;
	}

	/**
	 * Split `In` on WHOLE-WORD occurrences of `Word` (case-insensitive), preserving
	 * every other character verbatim, then join the pieces with a single space.
	 *
	 * Two independent cursors are required: SegStart (start of the current output
	 * segment, advances only on a genuine whole-word match) and SearchFrom (where the
	 * next Find begins, always advances past a hit). Reusing a single cursor and
	 * advancing it on a NON-whole-word hit (e.g. "or" inside "actors", "and" inside
	 * "command", "not" inside "node") silently DROPS the surrounding text.
	 * Whole-word-only boolean (and/or/not) stripping, so substrings inside tokens
	 * are preserved. Shared by the lexical and semantic channels from a single
	 * implementation.
	 */
	static FString Cl628_StripBoolWord(const FString& In, const TCHAR* Word)
	{
		TArray<FString> Out;
		const FString InLower = In.ToLower();
		const FString WordLower = FString(Word).ToLower();
		const int32 WordLen = WordLower.Len();
		int32 SegStart = 0;
		int32 SearchFrom = 0;
		while (WordLen > 0 && SearchFrom <= In.Len() - WordLen)
		{
			const int32 Hit = InLower.Find(WordLower, ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom);
			if (Hit == INDEX_NONE) { break; }
			const bool LeftBoundary  = (Hit == 0) || !FChar::IsAlnum(In[Hit - 1]);
			const bool RightBoundary = (Hit + WordLen >= In.Len()) || !FChar::IsAlnum(In[Hit + WordLen]);
			if (LeftBoundary && RightBoundary)
			{
				Out.Add(In.Mid(SegStart, Hit - SegStart));
				SegStart = Hit + WordLen;
			}
			SearchFrom = Hit + WordLen;
		}
		Out.Add(In.Mid(SegStart));
		return FString::Join(Out, TEXT(" "));
	}

	/** Tokenise into lowercase words, splitting on whitespace and common punctuation. */
	static void Tokenise(const FString& In, TArray<FString>& OutTokens)
	{
		OutTokens.Reset();
		if (In.IsEmpty()) { return; }

		const FString Lower = In.ToLower();
		FString Current;
		Current.Reserve(32);

		auto IsSeparator = [](TCHAR C) -> bool
		{
			if (C == TEXT(' ') || C == TEXT('\t') || C == TEXT('\n') || C == TEXT('\r'))
			{
				return true;
			}
			switch (C)
			{
				case TEXT('.'): case TEXT('-'): case TEXT('_'): case TEXT(','):
				case TEXT(':'): case TEXT(';'): case TEXT('/'): case TEXT('\\'):
				case TEXT('('): case TEXT(')'): case TEXT('['): case TEXT(']'):
				case TEXT('{'): case TEXT('}'):
					return true;
				default:
					return false;
			}
		};

		for (int32 i = 0; i < Lower.Len(); ++i)
		{
			const TCHAR C = Lower[i];
			if (IsSeparator(C))
			{
				if (Current.Len() >= 2) { OutTokens.Add(Current); }
				Current.Reset();
			}
			else if (FChar::IsAlnum(C))
			{
				Current.AppendChar(C);
			}
			// Any other character (quotes, *, +, %, punctuation that is not an
			// explicit separator) is STRIPPED within the token rather than kept.
			// This keeps emitted tokens strictly alphanumeric so the FTS5 MATCH
			// phrase form ("tok" / "tok"*) can never be malformed by an embedded
			// quote or wildcard, and so a hostile query like '/* "; drop' cannot
			// produce a broken MATCH expression. Shared with EnrichField on the
			// index-build path, so index and query tokenize identically.
		}
		if (Current.Len() >= 2) { OutTokens.Add(Current); }
	}

	/**
	 * Expand a tokenised query array in place using the forward + reverse
	 * abbreviation maps.
	 *
	 * Tokens derived from expansion are appended; the original tokens are kept
	 * unchanged.  No length filter is applied to expansion-derived tokens here
	 * (the caller applies min-length filtering to user-typed tokens before calling).
	 */
	static void ExpandQueryTokens(TArray<FString>& InOutTokens)
	{
		const TMap<FString, TArray<FString>>& Forward = GetForwardMap();
		const TMap<FString, TArray<FString>>& Reverse = GetReverseMap();

		TArray<FString> Additions;
		for (const FString& Tok : InOutTokens)
		{
			if (const TArray<FString>* Expansions = Forward.Find(Tok))
			{
				for (const FString& Exp : *Expansions) { Additions.AddUnique(Exp); }
			}
			if (const TArray<FString>* Abbrevs = Reverse.Find(Tok))
			{
				for (const FString& Abbr : *Abbrevs) { Additions.AddUnique(Abbr); }
			}
		}
		for (const FString& Add : Additions)
		{
			InOutTokens.AddUnique(Add);
		}
	}

	/**
	 * Take a raw field value and return it with abbreviation expansions and
	 * reverse abbreviations appended. The FTS5 tokenizer then indexes the
	 * enriched string so both the original term and its synonyms are findable.
	 *
	 * Example: "gas" -> "gas gameplay ability system" (forward)
	 * Example: "blueprint" -> "blueprint bp graph" (reverse)
	 */
	static FString EnrichField(const FString& FieldValue)
	{
		if (FieldValue.IsEmpty()) { return FieldValue; }

		const TMap<FString, TArray<FString>>& Forward = GetForwardMap();
		const TMap<FString, TArray<FString>>& Reverse = GetReverseMap();

		TArray<FString> Tokens;
		Tokenise(FieldValue, Tokens);

		TSet<FString> Additions;
		for (const FString& Tok : Tokens)
		{
			if (const TArray<FString>* Expansions = Forward.Find(Tok))
			{
				for (const FString& Exp : *Expansions)
				{
					if (Exp.Len() >= 2) { Additions.Add(Exp); }
				}
			}
			if (const TArray<FString>* Abbrevs = Reverse.Find(Tok))
			{
				for (const FString& Abbr : *Abbrevs)
				{
					if (Abbr.Len() >= 2) { Additions.Add(Abbr); }
				}
			}
		}

		if (Additions.Num() == 0) { return FieldValue; }

		FString Result = FieldValue;
		for (const FString& Add : Additions)
		{
			Result.AppendChar(TEXT(' '));
			Result.Append(Add);
		}
		return Result;
	}

	/**
	 * Flatten the JSON input schema's `properties` object into a single text
	 * string containing parameter names and enum values.
	 * This indexes parameter names so "set_actor_hidden" matches a query for
	 * "hidden" even when the word only appears in a param name or enum value.
	 */
	static FString FlattenParams(const TSharedPtr<FJsonObject>& InputSchema)
	{
		if (!InputSchema.IsValid()) { return FString(); }

		const TSharedPtr<FJsonObject>* PropertiesObj = nullptr;
		if (!InputSchema->TryGetObjectField(TEXT("properties"), PropertiesObj)
			|| !PropertiesObj || !(*PropertiesObj).IsValid())
		{
			return FString();
		}

		TArray<FString> Parts;

		for (const auto& PropPair : (*PropertiesObj)->Values)
		{
			// Parameter name
			Parts.Add(PropPair.Key);

			const TSharedPtr<FJsonObject>* PropSchema = nullptr;
			if (!PropPair.Value.IsValid() || !PropPair.Value->TryGetObject(PropSchema) || !PropSchema)
			{
				continue;
			}
			const TSharedPtr<FJsonObject>& Prop = *PropSchema;

			// Param DESCRIPTIONS are intentionally NOT indexed. Param-description
			// prose injects high-frequency generic tokens shared across hundreds of
			// tools (asset_path, session_id, "the path to...", etc.), creating broad
			// low-precision OR-matches that dilute bm25. We keep the param NAMES
			// (above) and enum values (short, discriminating) but drop the
			// free-text descriptions and nested-item descriptions.

			// Enum values (string enum lists) -- short, discriminating.
			const TArray<TSharedPtr<FJsonValue>>* EnumArr = nullptr;
			if (Prop->TryGetArrayField(TEXT("enum"), EnumArr) && EnumArr)
			{
				for (const TSharedPtr<FJsonValue>& Val : *EnumArr)
				{
					FString EnumStr;
					if (Val.IsValid() && Val->TryGetString(EnumStr) && !EnumStr.IsEmpty())
					{
						Parts.Add(EnumStr);
					}
				}
			}
		}

		return FString::Join(Parts, TEXT(" "));
	}
	// -------------------------------------------------------------------------
	// Raw BM25 query result (shared between FindNearest and FindNearestRawRanked).
	// -------------------------------------------------------------------------

	struct FRawBm25Result
	{
		FString Name;
		FString Category;
		double  Score;
	};

	// -------------------------------------------------------------------------
	// Steps 1-3: tokenize query, build OR MATCH expression, execute FTS5 query,
	// and collect raw bm25-ordered rows.
	//
	// Returns the row array (possibly empty). Returns an empty array and logs a
	// warning when the DB is unavailable or the query fails.
	//
	// Caller is expected to hold NO lock before calling -- this function acquires
	// GToolSearchLock internally for the Execute step.
	//
	// bm25() weight constants.
	// Column order in schema: name, keywords, category_operation, params,
	//                          description, examples
	// (UNINDEXED columns category and operation do not take weight arguments)
	// Integer weights avoid locale-dependent decimal formatting in FString::Printf
	// %g which could produce "1,5" on non-English locales and cause SQL parse errors.
	// Multiply the original floats (10, 8, 5, 3, 1.5, 1) by 2 to keep ratios.
	// -------------------------------------------------------------------------
	static const int32 Cl628_wName      = 20;
	static const int32 Cl628_wKeywords  = 16;
	static const int32 Cl628_wCatOp     = 10;
	static const int32 Cl628_wParams    =  6;
	static const int32 Cl628_wDesc      =  3;
	static const int32 Cl628_wExamples  =  2;

	/**
	 * Execute a bm25 FTS5 query for the given parameters and return raw ranked
	 * rows (score ASC, name ASC).  Steps 1-3 shared between FindNearest and
	 * FindNearestRawRanked so neither path can drift from the other.
	 *
	 * FetchLimit rows are fetched; the caller decides how many to consume.
	 * Returns empty when the index is not built, the query is empty/all-short-tokens,
	 * or the SQL execute fails.
	 */
	static TArray<FRawBm25Result> ExecuteRawBm25Query(
		const FString& Query,
		int32 FetchLimit,
		const FString& CategoryFilter)
	{
		TArray<FRawBm25Result> Rows;

		// ------------------------------------------------------------------
		// Step 1: Tokenize + expand (mirror matcher short-token handling)
		// ------------------------------------------------------------------

		TArray<FString> RawTokens;
		Tokenise(Query, RawTokens);

		// Split into kept (len > 2) and dropped (len <= 2) buckets.
		// The index is already enriched with abbreviation expansions at build time,
		// so querying raw tokens is sufficient for FTS5 to find matches through the
		// enriched fields. Query-time expansion is intentionally omitted here: adding
		// e.g. "create" -> "new add spawn place" to the query broadens matching so
		// much that the obviously-correct tool gets outranked by generic tools.
		TArray<FString> KeptTokens;
		TArray<FString> DroppedTokens;
		for (const FString& T : RawTokens)
		{
			(T.Len() > 2 ? KeptTokens : DroppedTokens).Add(T);
		}

		// Final query token set: long tokens preferred; short-only fallback.
		// Deduplicate: TSet::Add sets bAlreadyPresent=true when the element was
		// already in the set, so we add only when bAlreadyPresent=false.
		TArray<FString> QueryTokens;
		{
			TSet<FString> Seen;
			const TArray<FString>& Primary = (KeptTokens.Num() > 0) ? KeptTokens : DroppedTokens;
			for (const FString& T : Primary)
			{
				bool bAlreadyPresent = false;
				Seen.Add(T, &bAlreadyPresent);
				if (!bAlreadyPresent) { QueryTokens.Add(T); }
			}
		}

		if (QueryTokens.IsEmpty())
		{
			return Rows;
		}

		// ------------------------------------------------------------------
		// Step 2: Build a sanitized OR MATCH expression
		//   - Each token quoted as "tok" (exact) OR "tok"* (prefix, only for len>=4)
		//   - Terms joined by OR -- never pass raw agent text
		// ------------------------------------------------------------------

		TArray<FString> MatchTerms;
		MatchTerms.Reserve(QueryTokens.Num() * 2);
		for (const FString& Tok : QueryTokens)
		{
			// Quoted exact form: "tok"
			MatchTerms.Add(FString::Printf(TEXT("\"%s\""), *Tok));
			// Prefix form only for tokens >= 4 chars
			if (Tok.Len() >= 4)
			{
				MatchTerms.Add(FString::Printf(TEXT("\"%s\"*"), *Tok));
			}
		}

		const FString MatchExpr = FString::Join(MatchTerms, TEXT(" OR "));

		// ------------------------------------------------------------------
		// Step 3: Execute the FTS5 bm25 query with optional category filter
		//   Result columns: 0=raw_name, 1=category, 2=score(bm25)
		// ------------------------------------------------------------------

		// Format the bm25 weight arguments as a comma-separated list.
		const FString WeightArgs = FString::Printf(
			TEXT("tools, %d, %d, %d, %d, %d, %d"),
			Cl628_wName, Cl628_wKeywords, Cl628_wCatOp,
			Cl628_wParams, Cl628_wDesc, Cl628_wExamples);

		// Build SQL depending on whether a category filter is present.
		// Category filter applied INSIDE SQL (not post-filter) to avoid underfilling.
		const bool bHasCategory = !CategoryFilter.IsEmpty();

		// MatchExpr is embedded as a literal in the SQL rather than bound as ?N.
		// In SQLite 3.31.1, parameterized FTS5 MATCH expressions with complex phrase
		// queries (double-quoted terms, OR operators, wildcards) return 0 rows even
		// when the index is populated and an identical literal query succeeds.
		// The MatchExpr is produced entirely from tokenized user input (no raw string
		// passthrough), so embedding it as a literal is safe.  Tokenise() emits
		// strictly alphanumeric tokens (every non-alnum char is either a separator or
		// stripped within the token), so a token can never contain a single quote,
		// double quote, or wildcard. The single-quote doubling below is therefore a
		// defensive belt-and-suspenders against future tokenizer changes, not a
		// load-bearing escape for current input.
		FString MatchExprEscaped = MatchExpr.Replace(TEXT("'"), TEXT("''"));

		// Escape CategoryFilter for embedding as a SQL literal (same single-quote
		// doubling as MatchExprEscaped above).  In practice category values are
		// alpha-underscore tool categories (e.g. "chooser"), so no escaping is
		// needed -- the Replace is a safety net only.
		FString CategoryFilterEscaped = CategoryFilter.Replace(TEXT("'"), TEXT("''"));

		// Build the final SQL with all literals embedded.
		// Using Execute() (not FSQLitePreparedStatement::Step()) because SQLite
		// 3.31.1's FTS5 module returns 0 rows when a MATCH expression containing
		// FTS5 phrase/OR/wildcard syntax is supplied via sqlite3_bind_text(?).
		// The direct Execute() path drives sqlite3_step via the same internal
		// mechanism used by the working FSQLiteDatabase::Execute(sql, lambda)
		// calls elsewhere in this test suite.
		// Return (category || '_' || operation) as the raw tool name (col 0).
		// The 'name' column stores the enriched value which includes appended synonyms
		// and would break exact-name precedence matching. The UNINDEXED category and
		// operation columns store the verbatim values and concatenate to the tool name.
		FString SelectSql;
		if (bHasCategory)
		{
			SelectSql = FString::Printf(
				TEXT("SELECT (category || '_' || operation) AS raw_name, category, bm25(%s) AS score "
				     "FROM tools WHERE category = '%s' AND tools MATCH '%s' "
				     "ORDER BY score ASC, raw_name ASC LIMIT %d;"),
				*WeightArgs, *CategoryFilterEscaped, *MatchExprEscaped, FetchLimit);
		}
		else
		{
			SelectSql = FString::Printf(
				TEXT("SELECT (category || '_' || operation) AS raw_name, category, bm25(%s) AS score "
				     "FROM tools WHERE tools MATCH '%s' "
				     "ORDER BY score ASC, raw_name ASC LIMIT %d;"),
				*WeightArgs, *MatchExprEscaped, FetchLimit);
		}

		FScopeLock Lock(&GToolSearchLock);

		if (!GToolSearchDb || !GToolSearchDb->IsValid())
		{
			return Rows;
		}

		Rows.Reserve(FetchLimit);

		const int64 ExecRows = GToolSearchDb->Execute(*SelectSql,
			[&Rows](const FSQLitePreparedStatement& Stmt) -> ESQLitePreparedStatementExecuteRowResult
			{
				FRawBm25Result R;
				double Score = 0.0;
				Stmt.GetColumnValueByIndex(0, R.Name);
				Stmt.GetColumnValueByIndex(1, R.Category);
				Stmt.GetColumnValueByIndex(2, Score);
				R.Score = Score;
				Rows.Add(MoveTemp(R));
				return ESQLitePreparedStatementExecuteRowResult::Continue;
			});

		if (ExecRows == INDEX_NONE)
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[ToolSearchIndex] ExecuteRawBm25Query: Execute() failed for query '%s'"), *Query);
			Rows.Reset();
		}

		return Rows;
	}

}  // namespace Cl628IdxInternal

// ---------------------------------------------------------------------------
// FClaireonToolSearchIndex
// ---------------------------------------------------------------------------

namespace Cl628IdxInternal
{
	// Build the catalog entry list from the live FClaireonServer tool registry.
	// Skips the python_execute / tool_search meta tools. Returns true if a live
	// server was available (entries may still be empty if the registry is empty).
	static bool BuildEntriesFromLiveServer(TArray<FClaireonToolCatalogEntry>& OutEntries)
	{
		OutEntries.Reset();
		FClaireonServer* Server = FClaireonModule::Get().GetServer();
		if (!Server)
		{
			return false;
		}
		const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = Server->GetTools();
		OutEntries.Reserve(Tools.Num());
		for (const TPair<FString, TSharedPtr<IClaireonTool>>& Pair : Tools)
		{
			const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
			if (!Tool.IsValid()) { continue; }

			const FString ToolName = Tool->GetName();
			if (ToolName == TEXT("python_execute") || ToolName == TEXT("tool_search"))
			{
				continue;
			}

			FClaireonToolCatalogEntry Entry;
			Entry.Name        = ToolName;
			Entry.Description = Tool->GetFullDescription();
			Entry.Category    = Tool->GetCategory();
			Entry.Operation   = Tool->GetOperation();
			Entry.Keywords    = Tool->GetSearchKeywords();
			Entry.Params      = FlattenParams(Tool->GetInputSchema());

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

			OutEntries.Add(MoveTemp(Entry));
		}
		return true;
	}

	// Split an identifier-shaped tool name into space-separated words so a dense
	// encoder sees natural-language tokens. Splits on '_', '-', '.' (the separators
	// tool names use). Collapses repeated separators; never emits empty words.
	static FString NameAsWords(const FString& Name)
	{
		FString Out;
		Out.Reserve(Name.Len() + 4);
		bool bPrevWasChar = false;
		for (int32 i = 0; i < Name.Len(); ++i)
		{
			const TCHAR C = Name[i];
			if (C == TEXT('_') || C == TEXT('-') || C == TEXT('.'))
			{
				if (bPrevWasChar) { Out.AppendChar(TEXT(' ')); }
				bPrevWasChar = false;
			}
			else
			{
				Out.AppendChar(C);
				bPrevWasChar = true;
			}
		}
		Out.TrimEndInline();
		return Out;
	}
}  // namespace Cl628IdxInternal

FString FClaireonToolSearchIndex::BuildSemanticDocString(const TSharedPtr<IClaireonTool>& Tool)
{
	using namespace Cl628IdxInternal;

	if (!Tool.IsValid())
	{
		return FString();
	}

	// Shared field extraction -- the SAME getters BuildEntriesFromLiveServer pulls
	// for the FTS5 columns, so the lexical and semantic documents stay in lock-step.
	const FString Name        = Tool->GetName();
	const FString Category    = Tool->GetCategory();
	const FString Operation   = Tool->GetOperation();
	const TArray<FString> Keywords = Tool->GetSearchKeywords();
	const FString Description  = Tool->GetFullDescription();
	const FString ExampleUsage = Tool->GetExampleUsage();
	const FString Patterns     = Tool->GetPatterns();
	// Reuse the single-sourced param-name flattener (param NAMES + enum values;
	// no free-text param descriptions).
	const FString ParamNames   = FlattenParams(Tool->GetInputSchema());

	// Space-join the fields in the documented order, skipping empties so the
	// encoder never sees padding tokens from double spaces.
	TArray<FString> Parts;
	Parts.Reserve(8);

	const FString NameWords = NameAsWords(Name);
	if (!NameWords.IsEmpty())   { Parts.Add(NameWords); }
	if (!Category.IsEmpty())    { Parts.Add(Category); }
	if (!Operation.IsEmpty())   { Parts.Add(Operation); }
	if (Keywords.Num() > 0)     { Parts.Add(FString::Join(Keywords, TEXT(" "))); }
	if (!Description.IsEmpty()) { Parts.Add(Description); }
	if (!ExampleUsage.IsEmpty()){ Parts.Add(ExampleUsage); }
	if (!Patterns.IsEmpty())    { Parts.Add(Patterns); }
	if (!ParamNames.IsEmpty())  { Parts.Add(ParamNames); }

	return FString::Join(Parts, TEXT(" "));
}

bool FClaireonToolSearchIndex::EnsureBuilt()
{
	{
		FScopeLock Lock(&GToolSearchLock);
		if (GToolSearchDb && GToolSearchDb->IsValid())
		{
			return true;
		}
	}

	// DB not open: build entries from the live server (if any) and populate.
	// BuildCatalog opens the DB + creates the schema when needed and does an
	// atomic DELETE+INSERT, so it both opens and populates in one call.
	TArray<FClaireonToolCatalogEntry> Entries;
	Cl628IdxInternal::BuildEntriesFromLiveServer(Entries);
	BuildCatalog(Entries);

	return GToolSearchDb && GToolSearchDb->IsValid();
}

void FClaireonToolSearchIndex::RebuildFromLiveServer()
{
	// In-place atomic rebuild from the live registry: no DB close/reopen.
	// BuildCatalog opens the DB if needed, then DELETE+INSERT in one transaction.
	TArray<FClaireonToolCatalogEntry> Entries;
	if (Cl628IdxInternal::BuildEntriesFromLiveServer(Entries))
	{
		BuildCatalog(Entries);
	}
}

void FClaireonToolSearchIndex::BuildCatalog(const TArray<FClaireonToolCatalogEntry>& Entries)
{
	using namespace Cl628IdxInternal;

	FScopeLock Lock(&GToolSearchLock);

	// Open the database if not already open (idempotent schema check).
	if (!GToolSearchDb || !GToolSearchDb->IsValid())
	{
		// Database was not yet open; open without populating (EnsureBuilt logic).
		GToolSearchDb = MakeUnique<FSQLiteDatabase>();
		if (!GToolSearchDb->Open(TEXT(":memory:"), ESQLiteDatabaseOpenMode::ReadWriteCreate))
		{
			GToolSearchDb.Reset();
			UE_LOG(LogClaireon, Warning, TEXT("[ToolSearchIndex] BuildCatalog: failed to open in-memory DB"));
			return;
		}
		if (!GToolSearchDb->Execute(GCreateToolsTable))
		{
			GToolSearchDb->Close();
			GToolSearchDb.Reset();
			UE_LOG(LogClaireon, Warning, TEXT("[ToolSearchIndex] BuildCatalog: failed to create FTS5 table"));
			return;
		}
	}

	// Atomic replace: delete all rows then bulk-insert in a single transaction.
	if (!GToolSearchDb->Execute(TEXT("BEGIN;")))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[ToolSearchIndex] BuildCatalog: BEGIN failed"));
		return;
	}

	if (!GToolSearchDb->Execute(TEXT("DELETE FROM tools;")))
	{
		GToolSearchDb->Execute(TEXT("ROLLBACK;"));
		UE_LOG(LogClaireon, Warning, TEXT("[ToolSearchIndex] BuildCatalog: DELETE failed"));
		return;
	}

	// Prepared statement binding all 8 columns:
	// name, keywords, category_operation, params, description, examples, category, operation
	static const TCHAR* InsertSql =
		TEXT("INSERT INTO tools(name, keywords, category_operation, params, description, examples, category, operation) "
		     "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8);");

	FSQLitePreparedStatement InsertStmt;
	if (!InsertStmt.Create(*GToolSearchDb, InsertSql, ESQLitePreparedStatementFlags::Persistent))
	{
		GToolSearchDb->Execute(TEXT("ROLLBACK;"));
		UE_LOG(LogClaireon, Warning, TEXT("[ToolSearchIndex] BuildCatalog: failed to prepare INSERT statement"));
		return;
	}

	bool bAnyFailed = false;
	for (const FClaireonToolCatalogEntry& E : Entries)
	{
		// Build keywords string from array.
		const FString KeywordsStr = FString::Join(E.Keywords, TEXT(" "));

		// category_operation: enriched compound field.
		const FString CategoryOperation = E.Category + TEXT(" ") + E.Operation;

		// Six indexed columns: enrich with abbreviation expansions + reverse abbreviations.
		const FString NameEnriched             = EnrichField(E.Name);
		const FString KeywordsEnriched         = EnrichField(KeywordsStr);
		const FString CategoryOperationEnriched = EnrichField(CategoryOperation);
		const FString ParamsEnriched           = EnrichField(E.Params);
		const FString DescriptionEnriched      = EnrichField(E.Description);
		const FString ExamplesEnriched         = EnrichField(E.Examples);

		// Two UNINDEXED metadata columns: verbatim (not enriched -- these back
		// category filtering and grouped output, not FTS ranking).
		const FString& CategoryRaw  = E.Category;
		const FString& OperationRaw = E.Operation;

		InsertStmt.Reset();

		bool bOk = true;
		bOk = bOk && InsertStmt.SetBindingValueByIndex(1, NameEnriched);
		bOk = bOk && InsertStmt.SetBindingValueByIndex(2, KeywordsEnriched);
		bOk = bOk && InsertStmt.SetBindingValueByIndex(3, CategoryOperationEnriched);
		bOk = bOk && InsertStmt.SetBindingValueByIndex(4, ParamsEnriched);
		bOk = bOk && InsertStmt.SetBindingValueByIndex(5, DescriptionEnriched);
		bOk = bOk && InsertStmt.SetBindingValueByIndex(6, ExamplesEnriched);
		bOk = bOk && InsertStmt.SetBindingValueByIndex(7, CategoryRaw);
		bOk = bOk && InsertStmt.SetBindingValueByIndex(8, OperationRaw);

		if (!bOk)
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[ToolSearchIndex] BuildCatalog: binding failed for tool '%s'"), *E.Name);
			bAnyFailed = true;
			continue;
		}

		if (!InsertStmt.Execute())
		{
			UE_LOG(LogClaireon, Warning,
				TEXT("[ToolSearchIndex] BuildCatalog: INSERT execute failed for tool '%s'"), *E.Name);
			bAnyFailed = true;
		}
	}

	InsertStmt.Destroy();

	if (bAnyFailed)
	{
		GToolSearchDb->Execute(TEXT("ROLLBACK;"));
		UE_LOG(LogClaireon, Warning, TEXT("[ToolSearchIndex] BuildCatalog: rolled back due to INSERT failures"));
		return;
	}

	if (!GToolSearchDb->Execute(TEXT("COMMIT;")))
	{
		UE_LOG(LogClaireon, Warning, TEXT("[ToolSearchIndex] BuildCatalog: COMMIT failed"));
		return;
	}

	UE_LOG(LogClaireon, Verbose,
		TEXT("[ToolSearchIndex] BuildCatalog: indexed %d tools into FTS5"), Entries.Num());
}

TArray<FClaireonToolCatalogMatch> FClaireonToolSearchIndex::FindNearest(
	const FString& Query,
	int32 MaxResults,
	const FString& CategoryFilter)
{
	using namespace Cl628IdxInternal;

	TArray<FClaireonToolCatalogMatch> Out;
	if (MaxResults <= 0 || Query.IsEmpty())
	{
		return Out;
	}

	// We over-fetch to allow name-precedence reordering without re-querying.
	// Fetch at most MaxResults*4 candidates (capped) so the pin step has
	// enough material without ballooning for large MaxResults requests.
	const int32 FetchLimit = FMath::Min(MaxResults * 4, MaxResults + 32);
	const bool bHasCategory = !CategoryFilter.IsEmpty();

	// -------------------------------------------------------------------------
	// Steps 1-3: shared raw BM25 retrieval (tokenize, build MATCH, execute).
	// -------------------------------------------------------------------------
	const TArray<FRawBm25Result> Rows = ExecuteRawBm25Query(Query, FetchLimit, CategoryFilter);

	if (Rows.IsEmpty())
	{
		return Out;
	}

	// -------------------------------------------------------------------------
	// Step 7 + 8: Exact / near-exact name precedence + determinism
	//
	// Normalize the query for name-matching: lowercase, replace hyphens/
	// underscores with nothing (same treatment as the matcher).
	// -------------------------------------------------------------------------

	// Build a normalized form of the raw query for name comparison.
	FString NormalizedQuery = Query.ToLower();
	NormalizedQuery = NormalizedQuery.Replace(TEXT("-"), TEXT("")).Replace(TEXT("_"), TEXT(""));

	// Categorize rows into three buckets:
	//   0 = exact normalized name match  (pins to front)
	//   1 = Levenshtein <= 2 name match  (promoted next)
	//   2 = normal bm25 results
	// Within each bucket: score ASC, then name ASC.
	struct FScoredRow
	{
		int32  OriginalIndex;
		int32  PinBucket;   // 0=exact, 1=near, 2=normal
		double Score;
		FString Name;
	};
	TArray<FScoredRow> Scored;
	Scored.Reserve(Rows.Num());

	for (int32 i = 0; i < Rows.Num(); ++i)
	{
		const FRawBm25Result& R = Rows[i];

		// Normalize the candidate name the same way.
		FString NormName = R.Name.ToLower();
		NormName = NormName.Replace(TEXT("-"), TEXT("")).Replace(TEXT("_"), TEXT(""));

		int32 Bucket = 2;
		if (NormName == NormalizedQuery)
		{
			Bucket = 0;
		}
		else
		{
			const int32 Dist = FClaireonToolSearchIndex::DistanceBounded(NormName, NormalizedQuery, 2);
			if (Dist <= 2)
			{
				Bucket = 1;
			}
		}

		// Respect CategoryFilter for precedence pins too (consistent with matcher).
		if (Bucket < 2 && bHasCategory && !R.Category.Equals(CategoryFilter, ESearchCase::CaseSensitive))
		{
			Bucket = 2;
		}

		Scored.Add({ i, Bucket, R.Score, R.Name });
	}

	// Sort: bucket ASC, then score ASC (more-negative bm25 = better),
	// then name ASC for cross-platform tie stability.
	Scored.Sort([](const FScoredRow& A, const FScoredRow& B) -> bool
	{
		if (A.PinBucket != B.PinBucket) { return A.PinBucket < B.PinBucket; }
		if (A.Score     != B.Score)     { return A.Score     < B.Score; }
		return FCString::Strcmp(*A.Name, *B.Name) < 0;
	});

	// Populate output, taking up to MaxResults.
	const int32 Take = FMath::Min(MaxResults, Scored.Num());
	Out.Reserve(Take);
	for (int32 i = 0; i < Take; ++i)
	{
		const FRawBm25Result& R = Rows[Scored[i].OriginalIndex];
		FClaireonToolCatalogMatch M;
		M.Name     = R.Name;
		M.Category = R.Category;
		M.Score    = static_cast<float>(R.Score);
		// Score is the raw FTS5 bm25 value (negative; lower = better).
		// RankSource left at default (LexicalOnlyFallback) for existing callers.
		Out.Add(MoveTemp(M));
	}

	return Out;
}

TArray<FClaireonToolCatalogMatch> FClaireonToolSearchIndex::FindNearestRawRanked(
	const FString& Query,
	int32 MaxResults,
	const FString& CategoryFilter)
{
	using namespace Cl628IdxInternal;

	TArray<FClaireonToolCatalogMatch> Out;
	if (MaxResults <= 0 || Query.IsEmpty())
	{
		return Out;
	}

	// No over-fetch needed here: the caller (RRF) wants exactly MaxResults raw
	// bm25 candidates, and there is no post-retrieval reordering step.
	// We still apply a modest over-fetch so the RRF fusion step has enough
	// candidate pool material without a second query.
	// Convention: over-fetch factor 4 (same cap as FindNearest) for consistency.
	const int32 FetchLimit = FMath::Min(MaxResults * 4, MaxResults + 32);

	// -------------------------------------------------------------------------
	// Steps 1-3: shared raw BM25 retrieval (tokenize, build MATCH, execute).
	// No pin/sort pass -- the result is already ordered by score ASC, name ASC
	// from ExecuteRawBm25Query. This is the clean lexical input for RRF:
	// applying the Step 7-8 pin bucketing here would double-count the exact-name
	// signal when RRF runs its own pin pass on the fused list.
	//
	// FTS5 porter tokenization on both the index side ("porter unicode61" in
	// GCreateToolsTable) and the query side (FTS5 applies the same tokenizer to
	// MATCH expressions) ensures that "actors"/"actor", "worlds"/"world",
	// "spawns"/"spawn" all stem to the same form and match symmetrically.
	// Plural/singular recall regression tests lock this behavior.
	// -------------------------------------------------------------------------
	const TArray<FRawBm25Result> Rows = ExecuteRawBm25Query(Query, FetchLimit, CategoryFilter);

	if (Rows.IsEmpty())
	{
		return Out;
	}

	// Return up to MaxResults results in raw bm25 order (already sorted by
	// ExecuteRawBm25Query: score ASC, name ASC).
	const int32 Take = FMath::Min(MaxResults, Rows.Num());
	Out.Reserve(Take);
	for (int32 i = 0; i < Take; ++i)
	{
		const FRawBm25Result& R = Rows[i];
		FClaireonToolCatalogMatch M;
		M.Name      = R.Name;
		M.Category  = R.Category;
		M.Score     = static_cast<float>(R.Score);
		// LexicalOnlyFallback: score is raw bm25 (negative; lower/more-negative = better).
		// RRF fusion will consume this list and set RankSource = HybridRRF on the
		// fused output; results that survive fusion without entering the exact/near-exact
		// pin bucket stay LexicalOnlyFallback until RRF overwrites the field.
		M.RankSource = EClaireonRankSource::LexicalOnlyFallback;
		Out.Add(MoveTemp(M));
	}

	return Out;
}

namespace Cl628IdxInternal
{
	// -----------------------------------------------------------------------
	// RRF (Reciprocal Rank Fusion) tunables.
	//
	// File-local discriminator prefix (Cl628_) so these inline constants do not
	// collide with any other anon/namespace symbol under unity batching, and
	// `inline constexpr` (not namespace-scope `static constexpr`) to satisfy the
	// Linux-clang strict -Wunused-const-variable rule when a TU includes only a
	// subset.
	//
	// Score-polarity flip: raw bm25 is NEGATIVE / lower-better; the fused RRF
	// score is POSITIVE / higher-better. Consumers must read RankSource
	// (HybridRRF vs LexicalOnlyFallback) before interpreting Score.
	//
	// RRF formula as implemented:
	//   rrf(tool) = sum over each ranked list of  weight_list / (K + rank)
	// where:
	//   - rank is ZERO-BASED: the top item of a list has rank 0.
	//
	// Tuned RRF defaults for the bge-small-en-v1.5 embedder: semantic-leaning
	// (K=20, lexical 0.25, semantic 2.0) to favor the embedding signal while
	// keeping the lexical top-10 complement.
	//
	// K and the two weights live in a file-scope struct (Cl628_RrfParams)
	// initialized to these production defaults. ONLY the WITH_UNTESTED test setter
	// (FClaireonToolSearchIndex::SetRrfParamsForTest) mutates them; the harness is
	// single-threaded so no synchronization is needed. FindNearestHybrid and
	// Cl628_AccumulateRrf read these at call time, and the near-exact boost is
	// recomputed as 1.0/K at use (never a cached stale constant).
	struct FCl628_RrfParams
	{
		float K       = 20.0f;
		float WeightLexical  = 0.25f;
		float WeightSemantic = 2.0f;
	};

	// File-scope single instance. Default-constructed to the production defaults.
	// In a non-test build nothing ever mutates it, so it is effectively constant.
	static FCl628_RrfParams Cl628_RrfParams;

	// Accumulate weight/(K+rank) for one ranked list into FusedScores, keyed by
	// tool name. CategoryByName captures the category for any name first seen here
	// (both lists carry it; lexical and semantic agree on a tool's category).
	// K is read from the file-scope params at call time.
	static void Cl628_AccumulateRrf(
		const TArray<FClaireonToolCatalogMatch>& RankedList,
		float Weight,
		TMap<FString, float>& FusedScores,
		TMap<FString, FString>& CategoryByName)
	{
		const float CurrentK = Cl628_RrfParams.K;
		for (int32 Rank = 0; Rank < RankedList.Num(); ++Rank)
		{
			const FClaireonToolCatalogMatch& M = RankedList[Rank];
			const float Contribution = Weight / (CurrentK + static_cast<float>(Rank));
			FusedScores.FindOrAdd(M.Name, 0.0f) += Contribution;
			if (!CategoryByName.Contains(M.Name))
			{
				CategoryByName.Add(M.Name, M.Category);
			}
		}
	}

	// Normalize a name/query for exact + near-exact comparison: lowercase, strip
	// '-' and '_'. SAME treatment FindNearest's Step 7-8 pin pass applies, reused
	// here so the hybrid pin matches the legacy lexical pin byte-for-byte.
	static FString Cl628_NormalizeForName(const FString& In)
	{
		FString S = In.ToLower();
		S.ReplaceInline(TEXT("-"), TEXT(""));
		S.ReplaceInline(TEXT("_"), TEXT(""));
		return S;
	}
}  // namespace Cl628IdxInternal

TArray<FClaireonToolCatalogMatch> FClaireonToolSearchIndex::FindNearestHybrid(
	const FString& Query,
	int32 MaxResults,
	const FString& CategoryFilter)
{
	/**
	 * Hybrid BM25+Semantic+RRF search with a raw-bm25-recall gate.
	 *
	 * Semantic cosine is structurally always-on: every tool has an embedding, so
	 * FindNearestSemantic always returns Cl628_RrfCandidatePool candidates
	 * regardless of whether the query makes sense. A cosine floor therefore
	 * cannot be the honest "did anything match" signal -- it would let gibberish
	 * queries return semantically-padded noise.
	 *
	 * The RAW bm25 recall count IS the honest gate: BM25/FTS5 returns zero hits
	 * on gibberish (no token overlap with the corpus). So:
	 *
	 *   RawBm25Count == 0 && !bPreciseExactLookup  =>  return empty (no padding).
	 *   bPreciseExactLookup                         =>  exact tool + genuine lexical,
	 *                                                   no semantic, no RRF.
	 *   RawBm25Count >= 1, not exact lookup         =>  full BM25+semantic RRF fusion.
	 *
	 * Control flow:
	 *   Step 1  Guard.
	 *   Step 2  Lexical (raw bm25). Capture RawBm25Count BEFORE any pin injection.
	 *   Step 3  Precise exact-name lookup (whole normalized query == a tool name).
	 *   Step 4  Lexical gate: gibberish short-circuit.
	 *   Step 5  Precise-exact-lookup branch: exact + genuine lexical, no semantic.
	 *   Step 6  Normal branch: full hybrid RRF + near-exact boost (no exact pin here).
	 */

	using namespace Cl628IdxInternal;

	// -------------------------------------------------------------------------
	// Step 1: Guard.
	// -------------------------------------------------------------------------
	TArray<FClaireonToolCatalogMatch> Out;
	if (MaxResults <= 0 || Query.IsEmpty())
	{
		return Out;
	}

	// Candidate-pool depth fed into RRF from each channel. Deep enough that a tool
	// ranked outside MaxResults in one channel can still surface via the other.
	constexpr int32 Cl628_RrfCandidatePool = 100;

	// -------------------------------------------------------------------------
	// Step 2: Lexical channel (raw bm25, UNPINNED).
	//   FindNearestRawRanked takes the search-index lock internally.
	//   RawBm25Count is captured HERE -- before any pin injection -- because it is
	//   the gate signal (see rationale above).
	// -------------------------------------------------------------------------
	const TArray<FClaireonToolCatalogMatch> Lexical =
		FindNearestRawRanked(Query, Cl628_RrfCandidatePool, CategoryFilter);
	const int32 RawBm25Count = Lexical.Num();

	// -------------------------------------------------------------------------
	// Step 3: Precise exact-name lookup.
	//   A tool is the exact pin iff Cl628_NormalizeForName(ToolName) equals the
	//   whole normalized query -- NOT prose that merely contains a name. Lex-first
	//   tie-break for determinism. Meta tools (python_execute / tool_search) skipped
	//   consistent with BuildEntriesFromLiveServer. Exact bypass ignores CategoryFilter
	//   (operator rule: an agent typing the exact name must get it regardless).
	// -------------------------------------------------------------------------
	const FString NormalizedQuery = Cl628_NormalizeForName(Query);

	FString ExactPinName;      // empty == no precise exact match
	FString ExactPinCategory;

	if (!NormalizedQuery.IsEmpty())
	{
		if (FClaireonServer* Server = FClaireonModule::Get().GetServer())
		{
			const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = Server->GetTools();
			for (const TPair<FString, TSharedPtr<IClaireonTool>>& Pair : Tools)
			{
				const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
				if (!Tool.IsValid()) { continue; }
				const FString ToolName = Tool->GetName();
				// Skip meta tools (consistent with BuildEntriesFromLiveServer).
				if (ToolName == TEXT("python_execute") || ToolName == TEXT("tool_search"))
				{
					continue;
				}
				const FString NormName = Cl628_NormalizeForName(ToolName);
				if (NormName == NormalizedQuery)
				{
					// Lex-first tie-break when several names normalize the same.
					if (ExactPinName.IsEmpty() || ToolName < ExactPinName)
					{
						ExactPinName     = ToolName;
						ExactPinCategory = Tool->GetCategory();
					}
				}
			}
		}
	}

	const bool bPreciseExactLookup = !ExactPinName.IsEmpty();

	// -------------------------------------------------------------------------
	// Step 4: Lexical gate.
	//   Semantic cosine is structurally always-on so it MUST NOT rescue a
	//   lexically-empty non-lookup query -- that would pad gibberish with noise.
	//   If bm25 returned zero hits AND this is not a tool-name lookup, return empty.
	// -------------------------------------------------------------------------
	if (RawBm25Count == 0 && !bPreciseExactLookup)
	{
		return Out;
	}

	// -------------------------------------------------------------------------
	// Step 5: Precise-exact-lookup branch.
	//   Return the exact tool at rank 0 (RankSource=ExactPin, Score=1.0f pinned
	//   sentinel) followed by genuine lexical hits in their existing BM25 order,
	//   deduped. No semantic. No RRF. No near-exact boost. Category filter already
	//   applied by FindNearestRawRanked for the lexical tail; the exact pin bypasses
	//   it (operator rule). Truncate to MaxResults.
	// -------------------------------------------------------------------------
	if (bPreciseExactLookup)
	{
		// Pinned sentinel value. Fixed at 1.0f: not derived from RRF math, clearly
		// "this is a reserved pinned score" rather than a real fused RRF score.
		constexpr float ExactPinSentinel = 1.0f;

		Out.Reserve(FMath::Min(MaxResults, 1 + RawBm25Count));

		// Rank 0: exact pin.
		{
			FClaireonToolCatalogMatch M;
			M.Name       = ExactPinName;
			M.Category   = ExactPinCategory;
			M.Score      = ExactPinSentinel;
			M.RankSource = EClaireonRankSource::ExactPin;
			Out.Add(MoveTemp(M));
		}

		// Remaining ranks: genuine lexical hits in BM25 order, skipping the exact name.
		TSet<FString> Seen;
		Seen.Add(ExactPinName);
		for (const FClaireonToolCatalogMatch& L : Lexical)
		{
			if (Out.Num() >= MaxResults) { break; }
			if (Seen.Contains(L.Name)) { continue; }
			Seen.Add(L.Name);

			FClaireonToolCatalogMatch M;
			M.Name       = L.Name;
			M.Category   = L.Category;
			M.Score      = L.Score;
			M.RankSource = EClaireonRankSource::LexicalOnlyFallback;
			Out.Add(MoveTemp(M));
		}

		return Out;
	}

	// -------------------------------------------------------------------------
	// Step 6: Normal branch (RawBm25Count >= 1, not an exact-name lookup).
	//   Full hybrid RRF: BM25 + semantic fused via Reciprocal Rank Fusion, plus the
	//   near-exact identifier boost (soft, not a hard pin). No exact-pin injection
	//   here -- bPreciseExactLookup is false so there is no exact match to inject.
	// -------------------------------------------------------------------------

	// Semantic channel. FindNearestSemantic takes the embedding-index lock internally.
	// The two locks (search-index + embedding-index) are SEPARATE; called sequentially
	// (never nested) so there is no deadlock.
	const bool bSemanticReady = FClaireonToolEmbeddingIndex::IsReady();
	const TArray<FClaireonToolCatalogMatch> Semantic = bSemanticReady
		? FClaireonToolEmbeddingIndex::FindNearestSemantic(Query, Cl628_RrfCandidatePool, CategoryFilter)
		: TArray<FClaireonToolCatalogMatch>();

	// RRF fusion: sum weight/(K + rank) per tool across both ranked lists.
	TMap<FString, float>   FusedScores;
	TMap<FString, FString> CategoryByName;
	FusedScores.Reserve(Lexical.Num() + Semantic.Num());
	CategoryByName.Reserve(Lexical.Num() + Semantic.Num());

	Cl628_AccumulateRrf(Lexical,  Cl628_RrfParams.WeightLexical,  FusedScores, CategoryByName);
	Cl628_AccumulateRrf(Semantic, Cl628_RrfParams.WeightSemantic, FusedScores, CategoryByName);

	// Near-exact boost: identifier-shaped single-token query, DistanceBounded <= 2,
	// respects CategoryFilter. Additive soft boost (not a hard pin); sized at one
	// full top-rank lexical contribution so near-exact names reliably outrank mid-list
	// fusion results. Only one candidate earns the boost (lowest distance, lex-first).
	const bool bHasCategory = !CategoryFilter.IsEmpty();
	const bool bQueryIsIdentifierShaped = !NormalizedQuery.IsEmpty() && !NormalizedQuery.Contains(TEXT(" "));

	FString NearExactName;
	FString NearExactCategory;
	int32   NearExactDistance = INT32_MAX;

	if (bQueryIsIdentifierShaped)
	{
		if (FClaireonServer* Server = FClaireonModule::Get().GetServer())
		{
			const TMap<FString, TSharedPtr<IClaireonTool>>& Tools = Server->GetTools();
			for (const TPair<FString, TSharedPtr<IClaireonTool>>& Pair : Tools)
			{
				const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
				if (!Tool.IsValid()) { continue; }
				const FString ToolName = Tool->GetName();
				// Skip meta tools (consistent with BuildEntriesFromLiveServer).
				if (ToolName == TEXT("python_execute") || ToolName == TEXT("tool_search"))
				{
					continue;
				}
				const FString ToolCategory = Tool->GetCategory();
				// Near-exact respects the category filter (as FindNearest does).
				if (bHasCategory && !ToolCategory.Equals(CategoryFilter, ESearchCase::CaseSensitive))
				{
					continue;
				}
				const FString NormName = Cl628_NormalizeForName(ToolName);
				const int32 Dist = DistanceBounded(NormName, NormalizedQuery, 2);
				if (Dist <= 2
					&& (Dist < NearExactDistance
						|| (Dist == NearExactDistance && ToolName < NearExactName)))
				{
					NearExactDistance = Dist;
					NearExactName     = ToolName;
					NearExactCategory = ToolCategory;
				}
			}
		}
	}

	// Apply the near-exact additive boost. Ensure the tool is in the fused map even
	// if neither channel surfaced it (the boost can lift it into the result set).
	if (!NearExactName.IsEmpty())
	{
		// Near-exact (identifier-shaped, Levenshtein 1-2) additive RRF boost. NOT a
		// hard pin: added to the candidate's fused RRF score so it competes near the
		// top without unconditionally yanking to rank 0. Sized one full top-rank
		// lexical contribution (1.0 / K) so a near-exact name reliably outranks a
		// mid-list fusion result but never beats a genuine exact pin. Recomputed from
		// the live K at use -- never a cached stale constant.
		const float Cl628_RrfNearExactBoost = 1.0f / Cl628_RrfParams.K;
		FusedScores.FindOrAdd(NearExactName, 0.0f) += Cl628_RrfNearExactBoost;
		if (!CategoryByName.Contains(NearExactName))
		{
			CategoryByName.Add(NearExactName, NearExactCategory);
		}
	}

	// Materialize the fused list.
	//   RankSource: NearExactBoost for the boosted candidate; HybridRRF when semantic
	//   was ready, LexicalOnlyFallback otherwise.
	const EClaireonRankSource NonPinSource =
		bSemanticReady ? EClaireonRankSource::HybridRRF : EClaireonRankSource::LexicalOnlyFallback;

	// Pass 1: find the max fused score for documentation/debugging purposes (used if
	// needed by future sentinel logic; kept for parity with prior implementation).
	float MaxFusedScore = 0.0f;
	bool  bAnyScore = false;
	for (const TPair<FString, float>& Pair : FusedScores)
	{
		if (!bAnyScore || Pair.Value > MaxFusedScore)
		{
			MaxFusedScore = Pair.Value;
			bAnyScore = true;
		}
	}
	(void)MaxFusedScore;  // no sentinel needed in normal branch (no exact pin)
	(void)bAnyScore;

	TArray<FClaireonToolCatalogMatch> Fused;
	Fused.Reserve(FusedScores.Num());

	for (const TPair<FString, float>& Pair : FusedScores)
	{
		FClaireonToolCatalogMatch M;
		M.Name     = Pair.Key;
		M.Category = CategoryByName.FindRef(Pair.Key);
		M.Score    = Pair.Value;
		M.RankSource = (Pair.Key == NearExactName)
			? EClaireonRankSource::NearExactBoost
			: NonPinSource;
		Fused.Add(MoveTemp(M));
	}

	// Sort: Score DESC (higher = better -- polarity flip from raw bm25),
	// tie-break Name ASC for cross-platform determinism.
	Fused.Sort([](const FClaireonToolCatalogMatch& A, const FClaireonToolCatalogMatch& B) -> bool
	{
		if (A.Score != B.Score) { return A.Score > B.Score; }
		return FCString::Strcmp(*A.Name, *B.Name) < 0;
	});

	const int32 Take = FMath::Min(MaxResults, Fused.Num());
	Out.Reserve(Take);
	for (int32 i = 0; i < Take; ++i)
	{
		Out.Add(MoveTemp(Fused[i]));
	}
	return Out;
}

void FClaireonToolSearchIndex::Clear()
{
	FScopeLock Lock(&GToolSearchLock);

	if (GToolSearchDb && GToolSearchDb->IsValid())
	{
		GToolSearchDb->Execute(TEXT("DROP TABLE IF EXISTS tools;"));
		GToolSearchDb->Close();
	}
	GToolSearchDb.Reset();
}

#if WITH_UNTESTED
FSQLiteDatabase* FClaireonToolSearchIndex::GetDatabaseForTest()
{
	return GToolSearchDb.Get();
}

void FClaireonToolSearchIndex::SetRrfParamsForTest(float K, float LexW, float SemW)
{
	// Mutates the file-scope RRF params read by FindNearestHybrid + Cl628_AccumulateRrf.
	// Test-only; the harness is single-threaded so no lock is taken.
	// Always Reset after sweeping so other tests see the production defaults.
	Cl628IdxInternal::Cl628_RrfParams.K              = K;
	Cl628IdxInternal::Cl628_RrfParams.WeightLexical  = LexW;
	Cl628IdxInternal::Cl628_RrfParams.WeightSemantic = SemW;
}

void FClaireonToolSearchIndex::ResetRrfParamsForTest()
{
	Cl628IdxInternal::Cl628_RrfParams = Cl628IdxInternal::FCl628_RrfParams();
}
#endif

FString FClaireonToolSearchIndex::NormalizeQueryForRetrieval(const FString& Query)
{
	using namespace Cl628IdxInternal;

	if (Query.IsEmpty())
	{
		return Query;
	}

	// 1) Strip standalone boolean-operator tokens (whole-word, case-insensitive).
	//    SAME operator list + casing as the legacy Execute-layer strip.
	FString Work = Query;
	static const TCHAR* const BoolOps[] = { TEXT("AND"), TEXT("OR"), TEXT("NOT") };
	for (const TCHAR* Op : BoolOps)
	{
		Work = Cl628_StripBoolWord(Work, Op);
	}

	// 2) Drop boolean grouping punctuation that would otherwise survive into the
	//    embedded text (the lexical tokenizer already ignores these).
	Work.ReplaceInline(TEXT("("), TEXT(" "), ESearchCase::CaseSensitive);
	Work.ReplaceInline(TEXT(")"), TEXT(" "), ESearchCase::CaseSensitive);
	Work.ReplaceInline(TEXT("\""), TEXT(" "), ESearchCase::CaseSensitive);

	// 3) Collapse whitespace runs AND deduplicate words (case-insensitively,
	//    first-occurrence order preserved). This is what makes the boolean-decorated
	//    query normalize IDENTICALLY to the plain query: stripping "AND"/parens/quotes
	//    from "\"blueprint\" AND (chooser OR \"chooser\")" leaves multi-space runs
	//    AND a duplicated "chooser" (from "chooser OR \"chooser\""), which a dense
	//    encoder would otherwise embed differently from the plain "blueprint chooser".
	//    The lexical tokenizer already dedups internally, so a deduped query is a
	//    no-op there; for the semantic channel this collapse is what aligns the
	//    decorated and plain forms so they embed to the same vector
	//    (Claireon.ToolSearchBoolean.StripsAndOrNotOperators).
	TArray<FString> Words;
	Work.ParseIntoArray(Words, TEXT(" "), /*InCullEmpty=*/true);

	TArray<FString> Deduped;
	Deduped.Reserve(Words.Num());
	TSet<FString> Seen;
	for (const FString& W : Words)
	{
		bool bAlreadyPresent = false;
		Seen.Add(W.ToLower(), &bAlreadyPresent);
		if (!bAlreadyPresent)
		{
			Deduped.Add(W);
		}
	}
	return FString::Join(Deduped, TEXT(" "));
}

int32 FClaireonToolSearchIndex::DistanceBounded(const FString& A, const FString& B, int32 MaxDistance)
{
	// Bounded Levenshtein distance; returns MaxDistance + 1 when true distance exceeds the bound.
	const int32 LenA = A.Len();
	const int32 LenB = B.Len();

	// Fast path: length difference already exceeds bound.
	if (FMath::Abs(LenA - LenB) > MaxDistance)
	{
		return MaxDistance + 1;
	}

	// Use two-row DP with early-exit.
	TArray<int32> PrevRow, CurrRow;
	PrevRow.SetNumUninitialized(LenB + 1);
	CurrRow.SetNumUninitialized(LenB + 1);

	for (int32 j = 0; j <= LenB; ++j)
	{
		PrevRow[j] = j;
	}

	for (int32 i = 1; i <= LenA; ++i)
	{
		CurrRow[0] = i;
		int32 RowMin = CurrRow[0];

		for (int32 j = 1; j <= LenB; ++j)
		{
			const int32 Cost = (A[i - 1] == B[j - 1]) ? 0 : 1;
			CurrRow[j] = FMath::Min3(
				CurrRow[j - 1] + 1,
				PrevRow[j] + 1,
				PrevRow[j - 1] + Cost);
			RowMin = FMath::Min(RowMin, CurrRow[j]);
		}

		if (RowMin > MaxDistance)
		{
			return MaxDistance + 1;
		}

		Swap(PrevRow, CurrRow);
	}

	return PrevRow[LenB] <= MaxDistance ? PrevRow[LenB] : MaxDistance + 1;
}
