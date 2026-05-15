// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonToolCatalogMatcher.h"
#include "SquidTasks/Task.h"

// ===========================================================================
// Matcher tests (ClaireonToolCatalogMatcherTests)
// ===========================================================================
// Uses a fixed 20-tool fixture built in-test as a TArray<FClaireonToolCatalogEntry>.
// The fixture seeds at least one entry per shape (asset_search, blueprint_*,
// data_table_*, python_execute, pie_start) plus one ability_system_* entry so
// the "gas" abbreviation target exists.
// ===========================================================================

namespace ClaireonToolCatalogMatcherTestsHelpers
{
	/** Per-field fixture builder mirroring the Python harness's _build_field_text:
	 *  NameText receives both the raw name and its dot/underscore-tokenised form so
	 *  multi-part names contribute their components as separate index tokens. */
	static FClaireonToolCatalogEntry MakeEntry(const TCHAR* Name, const TCHAR* Desc, const TCHAR* Cat,
		const TCHAR* Operation = TEXT(""), const TCHAR* Keywords = TEXT(""))
	{
		FClaireonToolCatalogEntry X;
		X.Name = Name;
		X.Description = Desc;
		X.Category = Cat;
		const FString NameStr(Name);
		X.NameText = NameStr + TEXT(" ") + NameStr.Replace(TEXT("."), TEXT(" ")).Replace(TEXT("_"), TEXT(" "));
		X.CategoryText = Cat;
		X.DescriptionText = Desc;
		X.KeywordsText = Keywords;
		X.OperationText = Operation;
		return X;
	}

	/** Build the shared 20-entry fixture.  Keep this deterministic for test stability. */
	static TArray<FClaireonToolCatalogEntry> BuildFixtureTwenty()
	{
		const auto E = [](const TCHAR* Name, const TCHAR* Desc, const TCHAR* Cat)
		{
			return MakeEntry(Name, Desc, Cat);
		};

		TArray<FClaireonToolCatalogEntry> F;
		// asset_*
		F.Add(E(TEXT("asset_search"), TEXT("search assets by path and class filter"), TEXT("asset")));
		F.Add(E(TEXT("asset_delete"), TEXT("delete asset at path"), TEXT("asset")));
		// blueprint_*
		F.Add(E(TEXT("blueprint_compile"), TEXT("compile a blueprint asset"), TEXT("blueprint")));
		F.Add(E(TEXT("blueprint_diff"), TEXT("diff two blueprint assets"), TEXT("blueprint")));
		F.Add(E(TEXT("blueprint_list_graphs"), TEXT("list graphs on a blueprint"), TEXT("blueprint")));
		// data_table_*
		F.Add(E(TEXT("data_table_add_row"), TEXT("add a row to a data table"), TEXT("data_table")));
		F.Add(E(TEXT("data_table_list_rows"), TEXT("list rows in a data table"), TEXT("data_table")));
		// python
		F.Add(E(TEXT("python_execute"), TEXT("execute python code with claireon bridge"), TEXT("python")));
		// pie_*
		F.Add(E(TEXT("pie_start"), TEXT("start play in editor"), TEXT("pie")));
		F.Add(E(TEXT("pie_stop"), TEXT("stop play in editor"), TEXT("pie")));
		// bt / behavior tree
		F.Add(E(TEXT("behavior_tree_edit"), TEXT("edit a behavior tree asset"), TEXT("behavior_tree")));
		// gas / ability_system
		F.Add(E(TEXT("ability_system_add"), TEXT("add an ability to an ability system"), TEXT("ability_system")));
		F.Add(E(TEXT("ability_system_list"), TEXT("list abilities on an ability system"), TEXT("ability_system")));
		// log / file
		F.Add(E(TEXT("log_read"), TEXT("read a log file from disk"), TEXT("log")));
		F.Add(E(TEXT("file_read"), TEXT("read a file from disk"), TEXT("file")));
		// map
		F.Add(E(TEXT("map_open"), TEXT("open a map asset in the editor"), TEXT("map")));
		F.Add(E(TEXT("map_duplicate"), TEXT("duplicate a map asset"), TEXT("map")));
		// niagara
		F.Add(E(TEXT("niagara_emit"), TEXT("emit a niagara effect"), TEXT("niagara")));
		// live_coding
		F.Add(E(TEXT("live_coding_reload"), TEXT("reload live coding modules"), TEXT("live_coding")));
		// tool_search meta
		F.Add(E(TEXT("tool_search"), TEXT("search the tool catalog for a tool by name or description"), TEXT("tools")));
		return F;
	}
}

// ===========================================================================
// Build -> FindNearest returns a non-empty result
// ===========================================================================

UNTEST_UNIT(Claireon, ToolCatalogMatcher, BuildAndFindNearestNonEmpty)
{
	using namespace ClaireonToolCatalogMatcherTestsHelpers;

	FClaireonToolCatalogMatcher::Clear();
	FClaireonToolCatalogMatcher::BuildCatalog(BuildFixtureTwenty());

	TArray<FClaireonToolCatalogMatch> Matches = FClaireonToolCatalogMatcher::FindNearest(TEXT("asset_search"), 5);
	UNTEST_EXPECT_TRUE(Matches.Num() > 0);

	co_return;
}

// ===========================================================================
// Exact-name query returns the tool at rank 0 with the highest score
// ===========================================================================

UNTEST_UNIT(Claireon, ToolCatalogMatcher, ExactNameMatchRanksFirst)
{
	using namespace ClaireonToolCatalogMatcherTestsHelpers;

	FClaireonToolCatalogMatcher::Clear();
	FClaireonToolCatalogMatcher::BuildCatalog(BuildFixtureTwenty());

	TArray<FClaireonToolCatalogMatch> Matches = FClaireonToolCatalogMatcher::FindNearest(TEXT("blueprint_compile"), 5);
	UNTEST_ASSERT_TRUE(Matches.Num() > 0);
	UNTEST_EXPECT_STREQ(*Matches[0].Name, TEXT("blueprint_compile"));

	// The exact match must have the highest (strictly or weakly) score.
	for (int32 i = 1; i < Matches.Num(); ++i)
	{
		UNTEST_EXPECT_GE(Matches[0].Score, Matches[i].Score);
	}

	co_return;
}

// ===========================================================================
// Abbreviation queries surface tools of the expected category
// ===========================================================================

UNTEST_UNIT(Claireon, ToolCatalogMatcher, AbbreviationQueriesSurfaceCategory)
{
	using namespace ClaireonToolCatalogMatcherTestsHelpers;

	FClaireonToolCatalogMatcher::Clear();
	FClaireonToolCatalogMatcher::BuildCatalog(BuildFixtureTwenty());

	// "blueprint" should surface at least one blueprint_* tool inside the top-5.
	// NB: abbreviation expansion is performed Python-side by _ABBREVIATIONS in
	// mcp_tool_catalog.py.  At the C++ matcher level "bp" is a literal 2-char
	// token; we therefore exercise the full-form here.  The end-to-end "bp ->
	// blueprint_*" pipeline is covered by the Python harness tests.
	{
		TArray<FClaireonToolCatalogMatch> Top = FClaireonToolCatalogMatcher::FindNearest(TEXT("blueprint"), 5);
		bool bAnyBlueprint = false;
		for (const FClaireonToolCatalogMatch& M : Top)
		{
			if (M.Name.Contains(TEXT("blueprint_")))
			{
				bAnyBlueprint = true;
				break;
			}
		}
		UNTEST_EXPECT_TRUE(bAnyBlueprint);
	}

	// "data_table" should surface at least one data_table_* tool.
	{
		TArray<FClaireonToolCatalogMatch> Top = FClaireonToolCatalogMatcher::FindNearest(TEXT("data_table"), 5);
		bool bAny = false;
		for (const FClaireonToolCatalogMatch& M : Top)
		{
			if (M.Name.Contains(TEXT("data_table_")))
			{
				bAny = true;
				break;
			}
		}
		UNTEST_EXPECT_TRUE(bAny);
	}

	// "ability_system" should surface at least one ability_system_* tool.
	{
		TArray<FClaireonToolCatalogMatch> Top = FClaireonToolCatalogMatcher::FindNearest(TEXT("ability_system"), 5);
		bool bAny = false;
		for (const FClaireonToolCatalogMatch& M : Top)
		{
			if (M.Name.Contains(TEXT("ability_system_")))
			{
				bAny = true;
				break;
			}
		}
		UNTEST_EXPECT_TRUE(bAny);
	}

	co_return;
}

// ===========================================================================
// Clear() empties the catalog
// ===========================================================================

UNTEST_UNIT(Claireon, ToolCatalogMatcher, ClearEmptiesCatalog)
{
	using namespace ClaireonToolCatalogMatcherTestsHelpers;

	FClaireonToolCatalogMatcher::Clear();
	FClaireonToolCatalogMatcher::BuildCatalog(BuildFixtureTwenty());
	FClaireonToolCatalogMatcher::Clear();

	TArray<FClaireonToolCatalogMatch> Matches = FClaireonToolCatalogMatcher::FindNearest(TEXT("anything"), 5);
	UNTEST_EXPECT_EQ(Matches.Num(), 0);

	co_return;
}

// ===========================================================================
// Rebuild determinism -- identical rankings across clear-and-rebuild
// ===========================================================================

UNTEST_UNIT(Claireon, ToolCatalogMatcher, RebuildIsDeterministic)
{
	using namespace ClaireonToolCatalogMatcherTestsHelpers;

	FClaireonToolCatalogMatcher::Clear();
	FClaireonToolCatalogMatcher::BuildCatalog(BuildFixtureTwenty());
	TArray<FClaireonToolCatalogMatch> R1 = FClaireonToolCatalogMatcher::FindNearest(TEXT("asset"), 5);

	FClaireonToolCatalogMatcher::Clear();
	FClaireonToolCatalogMatcher::BuildCatalog(BuildFixtureTwenty());
	TArray<FClaireonToolCatalogMatch> R2 = FClaireonToolCatalogMatcher::FindNearest(TEXT("asset"), 5);

	UNTEST_ASSERT_TRUE(R1.Num() == R2.Num());
	for (int32 i = 0; i < R1.Num(); ++i)
	{
		UNTEST_EXPECT_STREQ(*R1[i].Name, *R2[i].Name);
		UNTEST_EXPECT_NEAR(R1[i].Score, R2[i].Score, 1e-6f);
	}

	co_return;
}

// ===========================================================================
// Union ranking: query "blueprint chooser proxy" must surface tools that
// match only one of the three tokens (not just tools that match all three).
// Locks in the per-word independent scoring semantics.
// ===========================================================================

UNTEST_UNIT(Claireon, ToolCatalogMatcher, UnionRankedReturnsDisjointTokenMatches)
{
	using namespace ClaireonToolCatalogMatcherTestsHelpers;

	// Custom fixture that has tools matching exactly one token each so we
	// can assert union semantics: the result set must include all three.
	const auto E = [](const TCHAR* Name, const TCHAR* Desc, const TCHAR* Cat)
	{
		return MakeEntry(Name, Desc, Cat);
	};
	TArray<FClaireonToolCatalogEntry> F;
	F.Add(E(TEXT("blueprint_compile"), TEXT("compile a blueprint asset"), TEXT("blueprint")));
	F.Add(E(TEXT("chooser_inspect"),    TEXT("inspect a chooser table"),  TEXT("chooser")));
	F.Add(E(TEXT("proxytable_inspect"), TEXT("inspect a proxy table"),    TEXT("proxytable")));
	// Filler entries so a 5-result top-K can still distinguish ranking.
	F.Add(E(TEXT("asset_search"),       TEXT("search assets"),            TEXT("asset")));
	F.Add(E(TEXT("map_open"),           TEXT("open a map"),               TEXT("map")));

	FClaireonToolCatalogMatcher::Clear();
	FClaireonToolCatalogMatcher::BuildCatalog(F);

	TArray<FClaireonToolCatalogMatch> Top = FClaireonToolCatalogMatcher::FindNearest(TEXT("blueprint chooser proxy"), 10);
	UNTEST_ASSERT_TRUE(Top.Num() >= 3);

	bool bSawBlueprint = false;
	bool bSawChooser   = false;
	bool bSawProxy     = false;
	for (const FClaireonToolCatalogMatch& M : Top)
	{
		if (M.Name == TEXT("blueprint_compile"))    { bSawBlueprint = true; }
		if (M.Name == TEXT("chooser_inspect"))      { bSawChooser   = true; }
		if (M.Name == TEXT("proxytable_inspect"))   { bSawProxy     = true; }
	}
	UNTEST_EXPECT_TRUE(bSawBlueprint);
	UNTEST_EXPECT_TRUE(bSawChooser);
	UNTEST_EXPECT_TRUE(bSawProxy);

	co_return;
}

// ===========================================================================
// Query-side >2-char cutoff -- short tokens drop unless every token is short.
// ===========================================================================

UNTEST_UNIT(Claireon, ToolCatalogMatcher, ThreeCharMinTermCutoff)
{
	using namespace ClaireonToolCatalogMatcherTestsHelpers;

	TArray<FClaireonToolCatalogEntry> F;
	F.Add(MakeEntry(TEXT("asset_create"),          TEXT("create a new asset"),      TEXT("asset")));
	F.Add(MakeEntry(TEXT("ai_decisions_inspect"),  TEXT("inspect ai decisions"),    TEXT("ai")));
	F.Add(MakeEntry(TEXT("blueprint_compile"),     TEXT("compile a blueprint"),     TEXT("blueprint")));

	FClaireonToolCatalogMatcher::Clear();
	FClaireonToolCatalogMatcher::BuildCatalog(F);

	// Sub-case A: query "ai create" -- "ai" is <=2 chars and must be dropped
	// in favour of "create"; asset_create matches (via NameText "create"
	// posting), ai_decisions_inspect must NOT (no "create" token in any of
	// its fields).
	{
		TArray<FClaireonToolCatalogMatch> Top = FClaireonToolCatalogMatcher::FindNearest(TEXT("ai create"), 5);
		bool bSawAssetCreate = false;
		bool bSawAiDecisions = false;
		for (const FClaireonToolCatalogMatch& M : Top)
		{
			if (M.Name == TEXT("asset_create"))         { bSawAssetCreate = true; }
			if (M.Name == TEXT("ai_decisions_inspect")) { bSawAiDecisions = true; }
		}
		UNTEST_EXPECT_TRUE(bSawAssetCreate);
		UNTEST_EXPECT_FALSE(bSawAiDecisions);
	}

	// Sub-case B: query "ai" alone -- all tokens are <=2 chars, so the kept-
	// empty fallback fires and "ai" is used. ai_decisions_inspect must surface.
	{
		TArray<FClaireonToolCatalogMatch> Top = FClaireonToolCatalogMatcher::FindNearest(TEXT("ai"), 5);
		bool bSawAiDecisions = false;
		for (const FClaireonToolCatalogMatch& M : Top)
		{
			if (M.Name == TEXT("ai_decisions_inspect")) { bSawAiDecisions = true; }
		}
		UNTEST_EXPECT_TRUE(bSawAiDecisions);
	}

	co_return;
}

// ===========================================================================
// Operation field participates in ranking with weight equal to Keywords (3).
// Tool A's Operation hit (weight 3) outranks Tool B's Description hit (weight 1).
// ===========================================================================

UNTEST_UNIT(Claireon, ToolCatalogMatcher, OperationFieldParticipatesInRanking)
{
	using namespace ClaireonToolCatalogMatcherTestsHelpers;

	TArray<FClaireonToolCatalogEntry> F;
	// Tool A: Operation = "create"; Description has no "create" token.
	F.Add(MakeEntry(TEXT("tool_a"), TEXT("does nothing of interest"), TEXT("alpha"),
		/*Operation=*/TEXT("create"), /*Keywords=*/TEXT("")));
	// Tool B: Operation = "inspect"; Description = "creates a thing" (Description hit).
	F.Add(MakeEntry(TEXT("tool_b"), TEXT("creates a thing"), TEXT("beta"),
		/*Operation=*/TEXT("inspect"), /*Keywords=*/TEXT("")));

	FClaireonToolCatalogMatcher::Clear();
	FClaireonToolCatalogMatcher::BuildCatalog(F);

	TArray<FClaireonToolCatalogMatch> Top = FClaireonToolCatalogMatcher::FindNearest(TEXT("create"), 5);
	UNTEST_ASSERT_TRUE(Top.Num() >= 2);
	UNTEST_EXPECT_STREQ(*Top[0].Name, TEXT("tool_a"));

	co_return;
}

#endif // WITH_UNTESTED
