// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// MAINTENANCE CONTRACT: When renaming or removing a tool, update this file in
// the SAME commit. Each row encodes a contract with agent users; silently
// breaking discoverability is worse than failing a unit test.

// 33-row discoverability ranking suite + 2 negative rows (34-35).
// Exercises FClaireonToolCatalogMatcher::FindNearest against a catalog built
// from the live server's registered tools. Validates that agent-facing
// queries surface the expected tool at or before MaxPosition (0-indexed) in
// the top-10 result list.
//
// Row 18 ("bp" single-token) asserts the short-token fallback path returns
// at least one bp_* tool -- position is unconstrained ("first page").
// Row 19 ("bp create") is the load-bearing test for Stage 020 step 4:
// the short-token whole-word path must rank bp_create at position <= 1.
// Rows 34-35 are negative rows that lock in the short-token substring-
// exclusion rule.

#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonToolCatalogMatcher.h"
#include "ClaireonModule.h"
#include "ClaireonServer.h"
#include "Tools/IClaireonTool.h"

namespace ClaireonToolDiscoverabilityTestsNS
{
	// File-local discriminator per feedback_anon_namespace_unity_collision.md.

	/** Build FClaireonToolCatalogMatcher from the live server's registered tools.
	 *  Mirrors the relevant portion of ClaireonTool_SearchTools::RebuildCatalog()
	 *  without the Python path. Returns true when at least one tool was indexed. */
	static bool BuildCatalogFromLiveServer()
	{
		FClaireonModule& Module = FClaireonModule::Get();
		if (!Module.IsServerRunning())
		{
			Module.StartServer();
		}
		FClaireonServer* Server = Module.GetServer();
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
			Entry.Description = Tool->GetDescription();
			Entry.Category    = Tool->GetCategory();
			Entry.Operation   = Tool->GetOperation();
			Entry.Keywords    = Tool->GetSearchKeywords();
			Entries.Add(MoveTemp(Entry));
		}

		FClaireonToolCatalogMatcher::BuildCatalog(Entries);
		return Entries.Num() > 0;
	}

	/** Log top-10 results for diagnostics when a row fails. */
	static void LogTop10(const FString& Query)
	{
		TArray<FClaireonToolCatalogMatch> Top = FClaireonToolCatalogMatcher::FindNearest(Query, 10);
		FString Msg = FString::Printf(TEXT("[ToolDiscoverability] Top-%d for query='%s': "), Top.Num(), *Query);
		for (int32 i = 0; i < Top.Num(); ++i)
		{
			Msg += FString::Printf(TEXT("[%d]%s "), i, *Top[i].Name);
		}
		UE_LOG(LogTemp, Display, TEXT("%s"), *Msg.TrimEnd());
	}

	/**
	 * Returns true when FClaireonToolCatalogMatcher::FindNearest(Query, 10) returns
	 * ExpectedName at index <= MaxPosition (0-indexed).
	 *
	 * UNTEST_* macros must NOT be called inside this helper
	 * (memory feedback_untest_macros_lambda_coroutine): they expand to co_return.
	 * The caller must call UNTEST_EXPECT_TRUE on the returned bool.
	 */
	static bool AssertSearchHitCheck(const FString& Query, const FString& ExpectedName, int32 MaxPosition)
	{
		TArray<FClaireonToolCatalogMatch> Top = FClaireonToolCatalogMatcher::FindNearest(Query, 10);
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
		TArray<FClaireonToolCatalogMatch> Top = FClaireonToolCatalogMatcher::FindNearest(Query, 10);
		for (const FClaireonToolCatalogMatch& M : Top)
		{
			if (M.Name.StartsWith(Prefix))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Returns true when NO top-10 result for Query contains ForbiddenSubstring
	 * in its name unless the name also has ExactWord as a whole underscore-token.
	 * Used for row 34 (negative).
	 *
	 * Example: Query="ed", ForbiddenSubstring="edit", ExactWord="ed"
	 * -- tools whose names contain "edit" but not the token "ed" must be absent.
	 */
	static bool AssertSearchMissCheck(const FString& Query, const FString& ForbiddenSubstring, const FString& ExactWord)
	{
		TArray<FClaireonToolCatalogMatch> Top = FClaireonToolCatalogMatcher::FindNearest(Query, 10);
		const FString ForbiddenLower = ForbiddenSubstring.ToLower();
		const FString ExactWordLower = ExactWord.ToLower();

		for (const FClaireonToolCatalogMatch& M : Top)
		{
			const FString NameLower = M.Name.ToLower();
			if (!NameLower.Contains(ForbiddenLower))
			{
				continue; // no forbidden substring -- this result is OK
			}
			// Name contains the forbidden substring. Allow if name has ExactWord
			// as a whole underscore-delimited token.
			TArray<FString> Tokens;
			NameLower.ParseIntoArray(Tokens, TEXT("_"), true);
			bool bHasExactToken = false;
			for (const FString& Token : Tokens)
			{
				if (Token == ExactWordLower)
				{
					bHasExactToken = true;
					break;
				}
			}
			if (!bHasExactToken)
			{
				UE_LOG(LogTemp, Error,
					TEXT("[ToolDiscoverability] AssertSearchMiss FAIL: query='%s', "
					     "tool '%s' contains forbidden substring '%s' without exact token '%s'."),
					*Query, *M.Name, *ForbiddenSubstring, *ExactWord);
				return false;
			}
		}
		return true;
	}

} // namespace ClaireonToolDiscoverabilityTestsNS

// ===========================================================================
// Rows 1-33: Discoverability ranking assertions.
// Each test builds the catalog from the live server's registered tools, then
// asserts the expected tool appears at or before MaxPosition in the top-10.
// ===========================================================================

// Row 1
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_CreateBp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("create bp");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_create"), 0);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 2
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_CreateBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("create blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_create"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 3
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_MakeNewBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("make new blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_create"), 2);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 4
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_OpenBp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("open bp");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_open"), 0);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 5
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_OpenBlueprintEditor, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("open blueprint editor");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_open"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 6
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_AddNodeBp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("add node bp");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_add_node"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 7
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_AddNodeBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("add node blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_add_node"), 1);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 8
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_CompileBp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("compile bp");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_compile"), 0);
	if (!bHit) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bHit);
	co_return;
}

// Row 9
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_CompileBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("compile blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_compile"), 1);
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

// Row 16
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_ApplyDeltaBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("apply delta blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_apply_delta"), 1);
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

// Row 19: "bp create" -- load-bearing test for Stage 020 step 4 short-token
// whole-word exact-match path. "bp" exact-matches name token "bp"; "create"
// exact-matches name token "create". bp_create gets both match weights and
// must rank at position <= 1. Failure here is a regression in Stage 020.
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

// Row 21
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_SetNodePropertyBp, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("set node property bp");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_set_node_property"), 1);
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
	const bool bHit = AssertSearchHitCheck(Query, TEXT("animgraph_add_node"), 2);
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

// Row 29: level_sequence_rebind_actor (#20781)
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

// Row 32
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_DuplicateBlueprint, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("duplicate blueprint");
	const bool bHit = AssertSearchHitCheck(Query, TEXT("bp_duplicate"), 1);
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

// ===========================================================================
// Negative rows (34-35): lock in short-token substring-exclusion rule.
// These rows pass ONLY when Stage 020 step 4 is in place.
// ===========================================================================

// Row 34: "ed" must NOT surface tools whose names contain "edit" but do NOT
// have "ed" as a whole underscore-delimited token. Locks in the substring-
// prohibition rule for short (<=2 char) tokens.
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_EdDoesNotMatchEdit, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());
	const FString Query = TEXT("ed");
	const bool bPass = AssertSearchMissCheck(Query, TEXT("edit"), TEXT("ed"));
	if (!bPass) { LogTop10(Query); }
	UNTEST_EXPECT_TRUE(bPass);
	co_return;
}

// Row 35: if any tool has "bp" ONLY in its description body (not in name or
// GetSearchKeywords), that tool must NOT appear in top-10 results for query "bp".
// As of this commit no such description-only "bp" tool exists in the catalog,
// so this test passes trivially as a forward-compatibility sentinel.
// DO NOT remove this test -- it activates the day someone adds "bp" to a
// description body without also adding it to GetSearchKeywords.
UNTEST_UNIT_OPTS(Claireon, ToolDiscoverability, Discoverability_BpInDescriptionExcluded, UNTEST_TIMEOUTMS(15000))
{
	using namespace ClaireonToolDiscoverabilityTestsNS;
	UNTEST_ASSERT_TRUE(BuildCatalogFromLiveServer());

	FClaireonModule& Module = FClaireonModule::Get();
	FClaireonServer* Server = Module.GetServer();
	UNTEST_ASSERT_PTR(Server);

	const TMap<FString, TSharedPtr<IClaireonTool>>& ToolsMap = Server->GetTools();
	const FString Query = TEXT("bp");
	TArray<FClaireonToolCatalogMatch> Top = FClaireonToolCatalogMatcher::FindNearest(Query, 10);

	// Collect names of tools that have "bp" in description but NOT in name
	// tokens or keywords (description-only tools).
	TSet<FString> DescOnlyBpTools;
	for (const TPair<FString, TSharedPtr<IClaireonTool>>& Pair : ToolsMap)
	{
		const TSharedPtr<IClaireonTool>& Tool = Pair.Value;
		if (!Tool.IsValid()) { continue; }
		const FString Name = Tool->GetName();
		if (Name == TEXT("python_execute") || Name == TEXT("tool_search")) { continue; }

		const FString NameLower = Name.ToLower();
		TArray<FString> NameTokens;
		NameLower.ParseIntoArray(NameTokens, TEXT("_"), true);
		const bool bBpInName = NameTokens.Contains(TEXT("bp"));

		bool bBpInKeywords = false;
		for (const FString& KW : Tool->GetSearchKeywords())
		{
			if (KW.ToLower() == TEXT("bp")) { bBpInKeywords = true; break; }
		}

		const bool bBpInDesc = Tool->GetDescription().ToLower().Contains(TEXT("bp"));

		if (bBpInDesc && !bBpInName && !bBpInKeywords)
		{
			DescOnlyBpTools.Add(Name);
		}
	}

	if (DescOnlyBpTools.Num() == 0)
	{
		// No description-only "bp" tools today -- passes trivially.
		UE_LOG(LogTemp, Display,
			TEXT("[ToolDiscoverability] Row35: no description-only-bp tools found; "
			     "test passes as forward-compatibility sentinel."));
		co_return;
	}

	bool bAnyViolation = false;
	for (const FClaireonToolCatalogMatch& M : Top)
	{
		if (DescOnlyBpTools.Contains(M.Name))
		{
			UE_LOG(LogTemp, Error,
				TEXT("[ToolDiscoverability] Row35 FAIL: '%s' has 'bp' only in description "
				     "but appeared in top-10 for query '%s'. Stage 020 step 4 "
				     "description-exclusion rule is broken."),
				*M.Name, *Query);
			bAnyViolation = true;
		}
	}
	UNTEST_EXPECT_TRUE(!bAnyViolation);

	co_return;
}

#endif // WITH_UNTESTED
