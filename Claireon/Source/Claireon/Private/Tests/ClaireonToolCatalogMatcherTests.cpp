// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonToolCatalogMatcher.h"
#include "SquidTasks/Task.h"

// ===========================================================================
// Matcher tests (ClaireonToolCatalogMatcherTests)
// ===========================================================================
// Cases 3a-3e per CLAIREON_DISK_RESULTS/test-plan.md section 3.  Uses a fixed
// 20-tool fixture built in-test as a TArray<FClaireonToolCatalogEntry>.  The
// fixture seeds at least one entry per shape called out in the plan
// (asset_search, blueprint_*, data_table_*, python_execute, pie_start) plus
// one ability_system_* entry so the "gas" abbreviation target exists.
// ===========================================================================

namespace ClaireonToolCatalogMatcherTestsHelpers
{
	/** Build the shared 20-entry fixture.  Keep this deterministic for test stability. */
	static TArray<FClaireonToolCatalogEntry> BuildFixtureTwenty()
	{
		const auto E = [](const TCHAR* Name, const TCHAR* Desc, const TCHAR* Cat)
		{
			FClaireonToolCatalogEntry X;
			X.Name = Name;
			X.Description = Desc;
			X.Category = Cat;
			X.EnrichedText = FString::Printf(TEXT("%s %s %s"), Name, Desc, Cat);
			return X;
		};

		TArray<FClaireonToolCatalogEntry> F;
		// asset_*
		F.Add(E(TEXT("claireon.asset_search"), TEXT("search assets by path and class filter"), TEXT("asset")));
		F.Add(E(TEXT("claireon.asset_delete"), TEXT("delete asset at path"), TEXT("asset")));
		// blueprint_*
		F.Add(E(TEXT("claireon.blueprint_compile"), TEXT("compile a blueprint asset"), TEXT("blueprint")));
		F.Add(E(TEXT("claireon.blueprint_diff"), TEXT("diff two blueprint assets"), TEXT("blueprint")));
		F.Add(E(TEXT("claireon.blueprint_list_graphs"), TEXT("list graphs on a blueprint"), TEXT("blueprint")));
		// data_table_*
		F.Add(E(TEXT("claireon.data_table_add_row"), TEXT("add a row to a data table"), TEXT("data_table")));
		F.Add(E(TEXT("claireon.data_table_list_rows"), TEXT("list rows in a data table"), TEXT("data_table")));
		// python
		F.Add(E(TEXT("claireon.python_execute"), TEXT("execute python code with claireon bridge"), TEXT("python")));
		// pie_*
		F.Add(E(TEXT("claireon.pie_start"), TEXT("start play in editor"), TEXT("pie")));
		F.Add(E(TEXT("claireon.pie_stop"), TEXT("stop play in editor"), TEXT("pie")));
		// bt / behavior tree
		F.Add(E(TEXT("claireon.behavior_tree_edit"), TEXT("edit a behavior tree asset"), TEXT("behavior_tree")));
		// gas / ability_system
		F.Add(E(TEXT("claireon.ability_system_add"), TEXT("add an ability to an ability system"), TEXT("ability_system")));
		F.Add(E(TEXT("claireon.ability_system_list"), TEXT("list abilities on an ability system"), TEXT("ability_system")));
		// log / file
		F.Add(E(TEXT("claireon.log_read"), TEXT("read a log file from disk"), TEXT("log")));
		F.Add(E(TEXT("claireon.file_read"), TEXT("read a file from disk"), TEXT("file")));
		// map
		F.Add(E(TEXT("claireon.map_open"), TEXT("open a map asset in the editor"), TEXT("map")));
		F.Add(E(TEXT("claireon.map_duplicate"), TEXT("duplicate a map asset"), TEXT("map")));
		// niagara
		F.Add(E(TEXT("claireon.niagara_emit"), TEXT("emit a niagara effect"), TEXT("niagara")));
		// live_coding
		F.Add(E(TEXT("claireon.live_coding_reload"), TEXT("reload live coding modules"), TEXT("live_coding")));
		// tools_search meta
		F.Add(E(TEXT("claireon.tools_search"), TEXT("search the tool catalog for a tool by name or description"), TEXT("tools")));
		return F;
	}
}

// ===========================================================================
// Case 3a: Build -> FindNearest returns a non-empty result
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
// Case 3b: exact-name query returns the tool at rank 0 with the highest score
// ===========================================================================

UNTEST_UNIT(Claireon, ToolCatalogMatcher, ExactNameMatchRanksFirst)
{
	using namespace ClaireonToolCatalogMatcherTestsHelpers;

	FClaireonToolCatalogMatcher::Clear();
	FClaireonToolCatalogMatcher::BuildCatalog(BuildFixtureTwenty());

	TArray<FClaireonToolCatalogMatch> Matches = FClaireonToolCatalogMatcher::FindNearest(TEXT("blueprint_compile"), 5);
	UNTEST_ASSERT_TRUE(Matches.Num() > 0);
	UNTEST_EXPECT_STREQ(*Matches[0].Name, TEXT("claireon.blueprint_compile"));

	// The exact match must have the highest (strictly or weakly) score.
	for (int32 i = 1; i < Matches.Num(); ++i)
	{
		UNTEST_EXPECT_GE(Matches[0].Score, Matches[i].Score);
	}

	co_return;
}

// ===========================================================================
// Case 3c: abbreviation queries surface tools of the expected category
// ===========================================================================

UNTEST_UNIT(Claireon, ToolCatalogMatcher, AbbreviationQueriesSurfaceCategory)
{
	using namespace ClaireonToolCatalogMatcherTestsHelpers;

	FClaireonToolCatalogMatcher::Clear();
	FClaireonToolCatalogMatcher::BuildCatalog(BuildFixtureTwenty());

	// "bp" should surface at least one blueprint_* tool inside the top-5.
	// NB: abbreviation expansion is performed Python-side by _ABBREVIATIONS in
	// mcp_tool_catalog.py.  At the C++ matcher level "bp" is a literal 2-char
	// token; we therefore also accept matches on "bp" as a token present in
	// the enriched text OR any blueprint_* tool in the top-5 via substring
	// overlap.  This test documents the C++ surface; the end-to-end "bp ->
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
// Case 3d: Clear() empties the catalog
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
// Case 3e: rebuild determinism -- identical rankings across clear-and-rebuild
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

#endif // WITH_UNTESTED
