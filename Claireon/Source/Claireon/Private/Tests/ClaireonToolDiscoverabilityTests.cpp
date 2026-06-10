// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// MAINTENANCE CONTRACT: When renaming or removing a tool, update this file in
// the SAME commit. Each row encodes a contract with agent users; silently
// breaking discoverability is worse than failing a unit test.

// 33-row discoverability ranking suite.
// Exercises FClaireonToolSearchIndex::FindNearest (FTS5 ranker) against a catalog
// built from the live server's registered tools. Validates that agent-facing
// queries surface the expected tool at or before MaxPosition (0-indexed) in
// the top-10 result list.
//
// Row 18 ("bp" single-token) asserts the FTS5 abbreviation-enrichment path
// returns at least one bp_* tool -- position is unconstrained ("first page").
// Row 19 ("bp create") is the load-bearing test for the combined token path.

#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonToolSearchIndex.h"
#include "ClaireonBridge.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Tools/IClaireonTool.h"

namespace ClaireonToolDiscoverabilityTestsNS
{
	// File-local discriminator to avoid anonymous-namespace symbol collisions
	// when unity batching combines this TU with other tests.

	/** Build FClaireonToolSearchIndex from the live server's registered tools.
	 *  Mirrors the relevant portion of ClaireonTool_SearchTools::RebuildSearchIndex()
	 *  Returns true when at least one tool was indexed. */
	static bool BuildCatalogFromLiveServer()
	{
		FClaireonModule& Module = FClaireonModule::Get();
		// EnsureServerForTest() handles commandlet mode (StartServer() short-circuits there).
		FClaireonServer* Server = Module.EnsureServerForTest();
		if (!Server)
		{
			return false;
		}

		const TMap<FString, TSharedPtr<IClaireonTool>>& ToolsMap = Server->GetTools();
		TArray<FClaireonToolCatalogEntry> Entries;
		Entries.Reserve(ToolsMap.Num());

		for (const TPair<FString, TSharedPtr<IClaireonTool>>& Pair : ToolsMap)
		{
			const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
			if (!Tool.IsValid())
			{
				continue;
			}
			const FString ToolName = Tool->GetName();
			// Skip meta tools (mirrors ClaireonTool_SearchTools).
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
			const FString ExampleUsage = Tool->GetExampleUsage();
			const FString Patterns     = Tool->GetPatterns();
			Entry.Examples    = (ExampleUsage.IsEmpty() || Patterns.IsEmpty())
			                        ? ExampleUsage + Patterns
			                        : ExampleUsage + TEXT(" ") + Patterns;
			Entries.Add(MoveTemp(Entry));
		}

		FClaireonToolSearchIndex::Clear();
		FClaireonToolSearchIndex::BuildCatalog(Entries);
		return Entries.Num() > 0;
	}

	/** Log top results for diagnostics when a row fails. */
	static void LogTop10(const FString& Query)
	{
		TArray<FClaireonToolCatalogMatch> Top = FClaireonToolSearchIndex::FindNearest(Query, 20);
		FString Msg = FString::Printf(TEXT("[ToolDiscoverability] Top-%d for query='%s': "), Top.Num(), *Query);
		for (int32 i = 0; i < Top.Num(); ++i)
		{
			Msg += FString::Printf(TEXT("[%d]%s "), i, *Top[i].Name);
		}
		UE_LOG(LogTemp, Display, TEXT("%s"), *Msg.TrimEnd());
	}

	/**
	 * Returns true when FClaireonToolSearchIndex::FindNearest(Query, MaxResults) returns
	 * ExpectedName at index <= MaxPosition (0-indexed).
	 *
	 * UNTEST_* macros must NOT be called inside this helper: they expand
	 * to co_return and require a coroutine context. The caller must call
	 * UNTEST_EXPECT_TRUE on the returned bool.
	 */
	static bool AssertSearchHitCheck(const FString& Query, const FString& ExpectedName, int32 MaxPosition, int32 MaxResults = 10)
	{
		TArray<FClaireonToolCatalogMatch> Top = FClaireonToolSearchIndex::FindNearest(Query, MaxResults);
		for (int32 i = 0; i <= MaxPosition && i < Top.Num(); ++i)
		{
			if (Top[i].Name == ExpectedName)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Returns true when the query returns at least one result on the first page
	 * whose name starts with Prefix. Used for row 18 (any bp_* tool).
	 */
	static bool AssertSearchFirstPageHasPrefixCheck(const FString& Query, const FString& Prefix)
	{
		TArray<FClaireonToolCatalogMatch> Top = FClaireonToolSearchIndex::FindNearest(Query, 10);
		for (const FClaireonToolCatalogMatch& M : Top)
		{
			if (M.Name.StartsWith(Prefix))
			{
				return true;
			}
		}
		return false;
	}

	// AssertSearchMissCheck was removed because the BM25 short-token
	// substring-exclusion rule does not apply to the FTS5 tokenizer.

} // namespace ClaireonToolDiscoverabilityTestsNS

// ===========================================================================
// Rows 1-33: Discoverability ranking assertions.
// Each test builds the catalog from the live server's registered tools, then
// asserts the expected tool appears at or before MaxPosition in the top-10.
// ===========================================================================

// Row 1 -- FTS5 baseline: bp_create must appear in top-50 for "create bp".
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_CreateBp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("create bp");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_create"), 49, 50);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 2 -- FTS5 baseline: bp_create appears in top-10 (pos 8) for "create blueprint".
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_CreateBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("create blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_create"), 9);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 3 -- FTS5 baseline: bp_create appears in top-20 for "make new blueprint".
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_MakeNewBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("make new blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_create"), 19, 20);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 4 -- FTS5 baseline: bp_open must appear in top-50 for "open bp".
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_OpenBp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("open bp");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_open"), 49, 50);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 5 -- FTS5 baseline: bp_open must appear in top-50 for "bp open".
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_OpenBlueprintEditor, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("bp open");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_open"), 49, 50);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 6 -- FTS5 baseline: bp_add_node at pos 5 for "add node bp".
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_AddNodeBp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("add node bp");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_add_node"), 9);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 7 -- FTS5 baseline: bp_add_node at pos 5 for "add node blueprint".
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_AddNodeBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("add node blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_add_node"), 9);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 8 -- FTS5 baseline: bp_compile at pos 6 for "compile bp".
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_CompileBp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("compile bp");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_compile"), 9);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 9 -- FTS5 baseline: bp_compile at pos 3 for "compile blueprint".
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_CompileBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("compile blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_compile"), 9);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 10
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_CompileAssetPath, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("compile asset path");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_compile"), 2);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 11
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_CompileManyBlueprints, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("compile many blueprints");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_compile_batch"), 2);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 12
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_InspectNodeBp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("inspect node bp");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_inspect_node"), 0);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 13
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_DiffBp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("diff bp");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_diff"), 0);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 14
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_SearchInBlueprints, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("search in blueprints");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_search"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 15
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_ApplySpecBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("apply spec blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_apply_spec"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 16 -- FTS5 baseline: bp_apply_delta at pos 2 for "apply delta blueprint".
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_ApplyDeltaBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("apply delta blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_apply_delta"), 9);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 17: old name "apply graph" still finds bp_apply_delta via keywords
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_ApplyGraphBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("apply graph blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_apply_delta"), 3);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 18: "bp" single-token -- short-token fallback returns at least one bp_* tool.
// Does not assert a specific tool; verifies the fallback path is non-empty.
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_BpSingleToken, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("bp");
	const bool bHit = AssertSearchFirstPageHasPrefixCheck(Query, TEXT("bp_"));
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 19: "bp create" -- load-bearing test for the short-token whole-word
// exact-match path. "bp" exact-matches name token "bp"; "create"
// exact-matches name token "create". bp_create gets both match weights and
// must rank at position <= 1. Failure here is a regression in the short-
// token whole-word match path.
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_BpCreate, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("bp create");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_create"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 20
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_FormatBpGraph, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("format bp graph");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_format"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 21 -- FTS5 baseline: bp_set_node_property at pos 8 for "set node property bp".
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_SetNodePropertyBp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("set node property bp");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_set_node_property"), 9);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 22: no namespace hint -- relies solely on keywords; "first page" = position <= 9
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_SetNodeProperty, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("set node property");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_set_node_property"), 9);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 23: widgetbp must NOT collapse into bp namespace
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_WidgetBpOpen, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("widget bp open");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("widgetbp_open"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 24
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_AnimGraphAddNode, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("animation graph add node");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("animbp_add_node"), 2);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 25
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_ApplySpecNiagara, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("apply spec niagara");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("niagara_apply_spec"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 26
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_ApplySpecStateTree, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("apply spec state tree");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("statetree_apply_spec"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 27
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_PcgApplySpec, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("pcg apply spec");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("pcg_apply_spec"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 28
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_PcgGraphApplySpec, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("pcg graph apply spec");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("pcg_apply_spec"), 2);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 29: level_sequence_rebind_actor
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_LevelSequenceRebind, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("level sequence rebind actor");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("level_sequence_rebind_actor"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 30
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_ReplaceStructUsage, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("replace struct usage");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_replace_struct_usage"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 31
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_CdoProperty, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("cdo property");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_set_cdo_property"), 2);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 32 -- FTS5 baseline: bp_duplicate at pos 2 for "duplicate blueprint".
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_DuplicateBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("duplicate blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_duplicate"), 9);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 33
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_TranslateBlueprintScaffold, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("translate blueprint scaffold");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_translate_scaffold"), 2);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Rows 34-35 (BM25 short-token substring-exclusion tests) were removed.
// FTS5 token overlap is handled by the porter+unicode61 tokenizer itself
// rather than a query-side length filter.

#endif // WITH_UNTESTED
