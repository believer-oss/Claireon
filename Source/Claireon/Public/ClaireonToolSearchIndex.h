// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

#include "Templates/SharedPointer.h"

// Forward declarations to avoid pulling in SQLite headers from the public interface.
class FSQLiteDatabase;
class IClaireonTool;

/**
 * One entry in the tool catalog.  Raw fields only -- the index owns
 * tokenisation and abbreviation expansion on insert.
 * See ClaireonToolCatalogAbbreviations.h for the synonym table.
 */
struct FClaireonToolCatalogEntry
{
	FString Name;
	FString Description;
	FString Category;
	FString Operation;
	/** Each keyword is one element; the index tokenises each independently. */
	TArray<FString> Keywords;
	/** Parameter documentation string (used by FClaireonToolSearchIndex). */
	FString Params;
	/** Usage examples string (used by FClaireonToolSearchIndex). */
	FString Examples;
};

/**
 * Identifies which retrieval signal produced a result and the score polarity.
 *
 * Agents and contract-snapshot assertions must read this field before
 * interpreting Score, because polarity is signal-dependent:
 *   - ExactPin / NearExactBoost: Score is a sentinel in (0, 1] (higher = closer).
 *   - HybridRRF: Score is an RRF value (positive; higher = better).
 *   - LexicalOnlyFallback: Score is a raw bm25 value (negative; lower = better).
 *
 */
enum class EClaireonRankSource : uint8
{
	/** The result is an exact (Levenshtein distance 0) name match; pinned to rank 0. */
	ExactPin,
	/** The result is a near-exact (distance 1-2) identifier-shaped match; boosted. */
	NearExactBoost,
	/** The result comes from RRF fusion of lexical + semantic signals. */
	HybridRRF,
	/** Semantic path unavailable; result is from the lexical-only path. */
	LexicalOnlyFallback,
};

/**
 * One scored match returned by FindNearest / FindNearestHybrid.
 *
 * Score polarity depends on RankSource; see EClaireonRankSource for the
 * per-source semantics. FindNearest always sets RankSource = LexicalOnlyFallback
 * and Score = raw bm25 (negative; lower / more-negative = better).
 */
struct FClaireonToolCatalogMatch
{
	FString Name;
	FString Category;
	float   Score = 0.0f;
	EClaireonRankSource RankSource = EClaireonRankSource::LexicalOnlyFallback;
};

/**
 * FTS5-backed full-text search index for the Claireon tool catalog.
 *
 * Stores tool metadata in an in-memory SQLite FTS5 virtual table with porter+unicode61
 * tokenization and bm25 ranking.
 *
 * Schema (in-memory):
 *   CREATE VIRTUAL TABLE IF NOT EXISTS tools USING fts5(
 *       name, keywords, category_operation, params, description, examples,
 *       category UNINDEXED, operation UNINDEXED,
 *       tokenize='porter unicode61');
 *
 * Thread safety: all public methods take an internal FCriticalSection.
 */
class CLAIREON_API FClaireonToolSearchIndex
{
public:
	/**
	 * Replace the index contents atomically.
	 * Opens the in-memory database if not already open, then inserts all entries.
	 */
	static void BuildCatalog(const TArray<FClaireonToolCatalogEntry>& Entries);

	/**
	 * Idempotent build trigger: if the database is not yet open, opens it and
	 * creates the FTS5 schema.  Does NOT populate rows; call BuildCatalog for that.
	 * Returns true if the database is valid after the call.
	 */
	static bool EnsureBuilt();

	/**
	 * Atomically rebuild the index in place from the live FClaireonServer tool
	 * registry (open the DB if needed, then DELETE+INSERT in one transaction --
	 * no DB close/reopen). Use this for the staleness-triggered refresh path
	 * (tool count change / OnToolsChanged dirty bit) so a rebuild does not pay
	 * the cost of dropping and recreating the database.
	 */
	static void RebuildFromLiveServer();

	/**
	 * Rank catalog entries against Query using FTS5 bm25.
	 * Returns at most MaxResults matches.  CategoryFilter restricts results to a
	 * specific category when non-empty (exact match on the UNINDEXED category column).
	 * Returns an empty array when the index is empty or no entries matched.
	 */
	static TArray<FClaireonToolCatalogMatch> FindNearest(
		const FString& Query,
		int32 MaxResults,
		const FString& CategoryFilter = {});

	/**
	 * Return the raw BM25-ranked candidate list WITHOUT the exact/near-exact name
	 * pinning applied by FindNearest(). This is the correct lexical input to RRF
	 * fusion: passing a pre-pinned list into RRF double-counts the pin signal.
	 *
	 * Results are ordered by bm25 score only (ascending; more-negative = better)
	 * with a name tie-break for cross-platform determinism.
	 * RankSource on each result is set to LexicalOnlyFallback.
	 *
	 * Returns an empty array when the index is empty or no entries matched.
	 */
	static TArray<FClaireonToolCatalogMatch> FindNearestRawRanked(
		const FString& Query,
		int32 MaxResults,
		const FString& CategoryFilter = {});

	/**
	 * Hybrid retrieval: fuse lexical (FindNearestRawRanked, unpinned bm25) and
	 * semantic (FClaireonToolEmbeddingIndex::FindNearestSemantic) signals via
	 * Reciprocal Rank Fusion, then apply exact/near-exact name pinning ONCE,
	 * outside RRF, here (so pinning is never double-applied by callers).
	 *
	 * RRF: rrf(tool) = sum over each list of weight / (K + rank),
	 * K = 60, rank ZERO-BASED, equal weights (lexical = semantic = 1.0). The fused
	 * Score is POSITIVE / higher-better (opposite polarity to raw bm25). Sorted
	 * Score DESC, tie-break Name ASC.
	 *
	 * Pin (outside RRF, once): the exact normalized-name match (over the FULL live
	 * registry, so it survives zero FTS/semantic recall) is forced to rank 0 with
	 * RankSource = ExactPin and a reserved sentinel Score (max fused RRF + 1.0).
	 * A near-exact (DistanceBounded <= 2, identifier-shaped query) match gets an
	 * additive RRF boost (NOT a hard pin) and RankSource = NearExactBoost.
	 * CategoryFilter is respected for the near-exact pin (exact bypasses it).
	 *
	 * When the semantic index is not ready (FClaireonToolEmbeddingIndex::IsReady()
	 * returns false), Semantic is empty, the fusion degenerates to lexical-only,
	 * and every non-pin result is tagged RankSource = LexicalOnlyFallback. The
	 * Execute() surface NEVER fails on a missing model.
	 */
	static TArray<FClaireonToolCatalogMatch> FindNearestHybrid(
		const FString& Query,
		int32 MaxResults,
		const FString& CategoryFilter = {});

	/**
	 * Drop the FTS5 table and close the database.
	 */
	static void Clear();

	/**
	 * Bounded Levenshtein distance with early-exit at MaxDistance.
	 * Returns MaxDistance + 1 as a sentinel when the true distance exceeds the bound.
	 * Used by ClaireonTool_SearchTools::Execute for near-exact-name precedence.
	 */
	static int32 DistanceBounded(const FString& A, const FString& B, int32 MaxDistance);

	/**
	 * Canonical retrieval-query normalization shared by EVERY retrieval channel so
	 * the lexical and semantic signals see the SAME query text.
	 *
	 * Strips standalone boolean-operator tokens (AND / OR / NOT, whole-word,
	 * case-insensitive -- matching the lexical strip), drops boolean grouping
	 * punctuation (parentheses, double quotes), and collapses runs of whitespace
	 * to single spaces (trimmed). It does NOT lowercase or strip identifier
	 * separators -- callers that need name-normalization do that separately.
	 *
	 * Why shared: the lexical channel tokenizes (so AND/OR/NOT and extra spaces are
	 * already harmless there), but the SEMANTIC channel embeds the RAW query string,
	 * so "blueprint AND (chooser OR \"chooser\")" would embed differently from
	 * "blueprint chooser" -- producing a different cosine ranking and breaking the
	 * "boolean-decorated query ranks identically to the plain query" contract
	 * (Claireon.ToolSearchBoolean.StripsAndOrNotOperators). Applying this in
	 * FindNearestSemantic (used by both the hybrid and the semantic-only harness)
	 * AND on the lexical retrieval path keeps the two channels in lock-step from a
	 * single source of truth.
	 */
	static FString NormalizeQueryForRetrieval(const FString& Query);

	/**
	 * Build the single per-tool document string the SEMANTIC index embeds.
	 *
	 * Single-sources the same live-registry fields the FTS5 insert path pulls in
	 * BuildEntriesFromLiveServer (GetName, GetCategory, GetOperation,
	 * GetSearchKeywords, GetFullDescription, GetExampleUsage + GetPatterns, and the
	 * flattened input-schema param names via the index-internal FlattenParams), so
	 * the lexical and semantic documents cannot drift. FlattenParams stays
	 * single-sourced in ClaireonToolSearchIndex.cpp; this is the one place both
	 * channels share it.
	 *
	 * Format (space-joined, in this order):
	 *   <name-as-words> <category> <operation> <keywords...> <description>
	 *   <examples/patterns> <flattened-param-names>
	 *
	 * The tool name is split on '_'/'-'/'.' into words so identifier tokens
	 * (e.g. "level_set_actor_property") embed as natural-language words. Empty
	 * fields are skipped (no double spaces).
	 *
	 * Returns an empty string when Tool is null. Unlike the FTS5 insert path this
	 * does NOT apply abbreviation enrichment: enrichment is a lexical-token trick
	 * that does not help a dense sentence encoder.
	 */
	static FString BuildSemanticDocString(const TSharedPtr<IClaireonTool>& Tool);

#if WITH_UNTESTED
	/**
	 * Returns the raw in-memory database pointer for low-level test assertions.
	 * Only available in test builds (WITH_UNTESTED).  Caller must call EnsureBuilt()
	 * first; returns nullptr if the database is not open.
	 */
	static FSQLiteDatabase* GetDatabaseForTest();

	/**
	 * Test-only RRF tuning hook. Overrides the RRF dampening constant K and the
	 * lexical/semantic fusion weights that FindNearestHybrid reads at call time.
	 * The near-exact boost is recomputed as 1.0/K at use, so tuning K also rescales
	 * the boost consistently. Defaults are 60 / 1.0 / 1.0 (the fixed production
	 * values); in a non-test build these are never mutated. Single-threaded harness
	 * use only -- no synchronization. Always pair with ResetRrfParamsForTest() so
	 * subsequent tests see the production defaults.
	 */
	static void SetRrfParamsForTest(float K, float LexW, float SemW);

	/** Restore the production RRF defaults (60 / 1.0 / 1.0). Test-only. */
	static void ResetRrfParamsForTest();
#endif
};
