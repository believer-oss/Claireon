// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

/**
 * One entry in the tool catalog.  The Python harness builds EnrichedText via
 * _enrich_text() (concatenation + synonym expansion) before passing the array
 * across the binding boundary.
 *
 * See CLAIREON_DISK_RESULTS/tool-catalog-rewrite.md for the full specification.
 */
struct FClaireonToolCatalogEntry
{
	FString Name;
	FString Description;
	FString Category;

	/** Built Python-side by _enrich_text(name, description, category). */
	FString EnrichedText;
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
};

/**
 * Dependency-free BM25-lite nearest-string matcher for the tool catalog.
 * Replaces the previous hybrid FTS5 + semantic-embedding search path.
 *
 * Scoring per tool-catalog-rewrite.md:
 *   raw_score       = (exact_hits * 2) + (prefix_hits * 1) + (fuzzy_hits * 0.5)
 *   normalised_score = raw_score / max(1, unique_query_token_count)
 *
 * Tie-break: Score desc, then Name asc (byte-wise FString::Compare) for
 * cross-platform determinism.
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
	/** Replace the catalog atomically.  Tokenises each entry's EnrichedText and rebuilds the inverted index. */
	static void BuildCatalog(const TArray<FClaireonToolCatalogEntry>& Entries);

	/**
	 * Rank catalog entries against Query.  Returns at most MaxResults matches,
	 * sorted by Score desc then Name asc.  Returns an empty array when the
	 * catalog is empty or when no candidate entries matched any query token.
	 */
	static TArray<FClaireonToolCatalogMatch> FindNearest(const FString& Query, int32 MaxResults);

	/** Empty the catalog and inverted index. */
	static void Clear();
};
