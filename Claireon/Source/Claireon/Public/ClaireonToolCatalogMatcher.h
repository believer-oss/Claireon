// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Misc/EnumClassFlags.h"

/**
 * Bitmask identifying which catalog field(s) produced an inverted-index posting
 * for a given token.  A single token can belong to multiple fields for the
 * same entry (e.g. `create` appearing in both Name and Operation for
 * `chooser_create`); the matcher then credits the MAX of contributing field
 * weights.
 */
enum class EFieldMask : uint8
{
	None        = 0,
	Name        = 1 << 0,
	Category    = 1 << 1,
	Keywords    = 1 << 2,
	Operation   = 1 << 3,
	Description = 1 << 4,
};
ENUM_CLASS_FLAGS(EFieldMask);

/**
 * One entry in the tool catalog.  Raw fields only -- the matcher owns
 * tokenisation and abbreviation expansion on both sides (BuildCatalog +
 * FindNearest).  See ClaireonToolCatalogAbbreviations.h for the synonym table.
 */
struct FClaireonToolCatalogEntry
{
	FString Name;
	FString Description;
	FString Category;
	FString Operation;
	/** Each keyword is one element; the matcher tokenises each independently
	 *  (no separator-format negotiation between caller and matcher). */
	TArray<FString> Keywords;
};

/**
 * One scored match returned by FindNearest.  Score is normalised to the
 * number of unique query tokens so small queries don't inflate against
 * longer queries in the same ranking.
 */
struct FClaireonToolCatalogMatch
{
	FString Name;
	FString Category;
	float   Score = 0.0f;
	/** Count of distinct query tokens that contributed any hit to this entry. */
	int32   TokensMatched = 0;
};

/**
 * Dependency-free BM25-lite nearest-string matcher for the tool catalog.
 * Replaces the previous hybrid FTS5 + semantic-embedding search path.
 *
 * Scoring:
 *   Per-hit contribution = MAX_OVER_FIELDS(weight_for_field) * (exact?2.0 : prefix?1.0 : fuzzy?0.5)
 *   Field weights: Name=8, Category=4, Keywords=3, Operation=3, Description=1.
 *   Final entry score is the sum over all distinct query tokens that contributed,
 *   normalised by max(1, unique_query_token_count).
 *
 * Tie-break: DistinctQueryTokensMatched desc, then Score desc, then Name asc
 * (byte-wise FString::Compare) for cross-platform determinism.
 *
 * All public methods are expected to be called on the game thread; an
 * internal FCriticalSection is held as belt-and-suspenders because the
 * Python-binding entry points could in theory be invoked off-thread if
 * CPython ever releases the GIL for a blocking call, and the matcher's
 * static storage is shared across Python REPL turns.
 */
class CLAIREON_API FClaireonToolCatalogMatcher
{
public:
	/** Replace the catalog atomically.  Tokenises each entry's per-field text strings and rebuilds the inverted index. */
	static void BuildCatalog(const TArray<FClaireonToolCatalogEntry>& Entries);

	/**
	 * Idempotent build trigger: if the catalog is currently empty, populates it
	 * from the live FClaireonServer tool registry (skipping the python_execute
	 * and tool_search meta tools).  Safe to call repeatedly; no-op when the
	 * catalog already has entries.  Used by python_execute hint nudges on the
	 * cold path (first invocation after editor launch, before any tool_search
	 * has rebuilt the catalog).  Returns true if the catalog has any entries
	 * after the call.
	 */
	static bool EnsureBuilt();

	/**
	 * Rank catalog entries against Query.  Returns at most MaxResults matches,
	 * sorted by DistinctQueryTokensMatched desc, then Score desc, then Name asc.
	 * Returns an empty array when the catalog is empty or when no candidate
	 * entries matched any query token.
	 */
	static TArray<FClaireonToolCatalogMatch> FindNearest(const FString& Query, int32 MaxResults);

	/** Empty the catalog and inverted index. */
	static void Clear();

	/**
	 * Bounded Levenshtein distance with early-exit at MaxDistance.  Returns
	 * MaxDistance + 1 as a sentinel when the true distance exceeds the bound.
	 * Exposed publicly so ClaireonTool_SearchTools::Execute can implement
	 * near-exact-name precedence without re-deriving the helper.
	 */
	static int32 DistanceBounded(const FString& A, const FString& B, int32 MaxDistance);
};
