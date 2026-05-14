// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for GetSearchKeywords coverage on top-30 tools (P3, Stage 009).
// Validates two acceptance criteria:
//   1. Each of the 30 picklist tools returns >= 3 search keywords.
//   2. Representative synonyms appear in the documented target tool's
//      keyword list, so the tools_search ranker has the vocabulary it
//      needs to surface the target. (Direct ranker invocation is covered
//      by ClaireonToolCatalogMatcherTests; here we assert the source data.)

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"

#include "Tools/ClaireonBehaviorTreeTool_Open.h"
#include "Tools/ClaireonBlackboardTool_Open.h"
#include "Tools/ClaireonEQSTool_Open.h"
#include "Tools/ClaireonLevelSequenceTool_Open.h"
#include "Tools/ClaireonNiagaraTool_Open.h"
#include "Tools/ClaireonPCGGraphTool_Open.h"
#include "Tools/ClaireonStateTreeTool_Open.h"
#include "Tools/ClaireonWidgetBPTool_Open.h"

#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/ClaireonBlueprintGraphTool_ConnectPins.h"
#include "Tools/ClaireonBlueprintGraphTool_SetPinValue.h"
#include "Tools/ClaireonBlueprintGraphTool_SelectPin.h"
#include "Tools/ClaireonBlueprintGraphTool_AddVariable.h"
#include "Tools/ClaireonBlueprintGraphTool_Save.h"
#include "Tools/ClaireonBlueprintGraphTool_Format.h"
#include "Tools/ClaireonBlueprintGraphTool_Compile.h"
#include "Tools/ClaireonBlueprintGraphTool_Close.h"

#include "Tools/ClaireonTool_BlueprintCompile.h"
#include "Tools/ClaireonTool_FormatBlueprintGraph.h"
#include "Tools/ClaireonTool_SearchTools.h"
#include "Tools/ClaireonTool_ExecutePython.h"
#include "Tools/ClaireonTool_ProxyTableInspect.h"
#include "Tools/ClaireonTool_ChooserInspect.h"
#include "Tools/ClaireonTool_AnimGraphInspect.h"
#include "Tools/ClaireonTool_ProxyAssetInspect.h"
#include "Tools/ClaireonTool_SearchInBlueprints.h"
#include "Tools/ClaireonTool_GetBlueprintGraph.h"
#include "Tools/ClaireonTool_GetBlueprintProperties.h"
#include "Tools/ClaireonTool_BlueprintDiff.h"

namespace SearchKeywordsTestHelpers
{
	template <typename ToolT>
	bool ValidateMinimumKeywordCount(const TCHAR* ExpectedNameSubstr, int32 MinKeywords = 3)
	{
		ToolT Tool;
		const FString Name = Tool.GetName();
		const TArray<FString> Keywords = Tool.GetSearchKeywords();
		if (ExpectedNameSubstr && !Name.Contains(ExpectedNameSubstr))
		{
			UE_LOG(LogTemp, Error, TEXT("[SearchKeywords] Tool name '%s' missing expected substring '%s'"), *Name, ExpectedNameSubstr);
			return false;
		}
		if (Keywords.Num() < MinKeywords)
		{
			UE_LOG(LogTemp, Error, TEXT("[SearchKeywords] Tool '%s' has %d keywords (expected >= %d)"), *Name, Keywords.Num(), MinKeywords);
			return false;
		}
		// Disallow duplicates and over-broad keywords.
		TSet<FString> Seen;
		for (const FString& K : Keywords)
		{
			const FString Lower = K.ToLower();
			if (Lower == TEXT("claireon") || Lower == TEXT("tool") || Lower == TEXT("mcp"))
			{
				UE_LOG(LogTemp, Error, TEXT("[SearchKeywords] Tool '%s' uses over-broad keyword '%s'"), *Name, *K);
				return false;
			}
			bool bAlreadyIn = false;
			Seen.Add(Lower, &bAlreadyIn);
			if (bAlreadyIn)
			{
				UE_LOG(LogTemp, Error, TEXT("[SearchKeywords] Tool '%s' has duplicate keyword '%s'"), *Name, *K);
				return false;
			}
		}
		return true;
	}

	template <typename ToolT>
	bool KeywordsContain(const TCHAR* RequiredKeyword)
	{
		ToolT Tool;
		const TArray<FString> Keywords = Tool.GetSearchKeywords();
		for (const FString& K : Keywords)
		{
			if (K.Equals(RequiredKeyword, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		UE_LOG(LogTemp, Error, TEXT("[SearchKeywords] Tool '%s' missing expected keyword '%s'"), *Tool.GetName(), RequiredKeyword);
		return false;
	}
}

// ---------------------------------------------------------------------------
// 1: Per-tool keyword count for all 30 P3 picklist tools.
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, SearchKeywords, TierA_SessionEntryPoints)
{
	using namespace SearchKeywordsTestHelpers;
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonBehaviorTreeTool_Open>(TEXT("behaviortree_open")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonBlackboardTool_Open>(TEXT("blackboard_open")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonTool_BlueprintCompile>(TEXT("blueprint_compile")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonTool_FormatBlueprintGraph>(TEXT("blueprint_format_graph")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonEQSTool_Open>(TEXT("eqs_open")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonLevelSequenceTool_Open>(TEXT("level_sequence_open")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonNiagaraTool_Open>(TEXT("niagara_open")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonPCGGraphTool_Open>(TEXT("pcg_open")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonStateTreeTool_Open>(TEXT("statetree_open")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonWidgetBPTool_Open>(TEXT("widgetbp_open")));
	co_return;
}

UNTEST_UNIT(Claireon, SearchKeywords, TierB_BlueprintGraphSlate)
{
	using namespace SearchKeywordsTestHelpers;
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonBlueprintGraphTool_Open>(TEXT("blueprint_graph_open")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonBlueprintGraphTool_AddNode>(TEXT("blueprint_graph_add_node")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonBlueprintGraphTool_ConnectPins>(TEXT("blueprint_graph_connect_pins")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonBlueprintGraphTool_SetPinValue>(TEXT("blueprint_graph_set_pin_value")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonBlueprintGraphTool_SelectPin>(TEXT("blueprint_graph_select_pin")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonBlueprintGraphTool_AddVariable>(TEXT("blueprint_graph_add_variable")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonBlueprintGraphTool_Save>(TEXT("blueprint_graph_save")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonBlueprintGraphTool_Format>(TEXT("blueprint_graph_format")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonBlueprintGraphTool_Compile>(TEXT("blueprint_graph_compile")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonBlueprintGraphTool_Close>(TEXT("blueprint_graph_close")));
	co_return;
}

UNTEST_UNIT(Claireon, SearchKeywords, TierC_ReadInspectDiscovery)
{
	using namespace SearchKeywordsTestHelpers;
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonTool_SearchTools>(TEXT("tools_search")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonTool_ExecutePython>(TEXT("python_execute")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonTool_ProxyTableInspect>(TEXT("proxytable_inspect")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonTool_ChooserInspect>(TEXT("chooser_inspect")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonTool_AnimGraphInspect>(TEXT("animgraph_inspect")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonTool_ProxyAssetInspect>(TEXT("proxyasset_inspect")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonTool_SearchInBlueprints>(TEXT("blueprint_search")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonTool_GetBlueprintGraph>(TEXT("blueprint_get_graph")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonTool_GetBlueprintProperties>(TEXT("blueprint_get_properties")));
	UNTEST_EXPECT_TRUE(ValidateMinimumKeywordCount<ClaireonTool_BlueprintDiff>(TEXT("blueprint_diff")));
	co_return;
}

// ---------------------------------------------------------------------------
// 2: Synonym -> target-tool keyword presence.
//
// These mappings document the synonym/target intent from P3 acceptance #3.
// Direct ranker validation lives in ClaireonToolCatalogMatcherTests; here we
// guarantee that the source-of-truth keyword list contains the words the
// matcher will index. If the matcher integrates the Python _ABBREVIATIONS
// table (which it does at query time), the presence of any one keyword
// from the synonym phrase is sufficient for top-N rank.
//
// Substitutions vs. P3:
//   - 'bt task' -> claireon.behaviortree_open (P3 cited claireon.behaviortree_edit
//      which is a session-name constant, not a registered tool; nearest
//      registered representative is _open).
//   - 'VB chain' / 'upperbody' synonyms target skeleton/anim tools outside
//      the 30-tool picklist; they are intentionally NOT asserted here so the
//      test stays scoped to Stage 009 deliverables. They remain in scope for
//      the broader Stage 013 description-audit lint.
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, SearchKeywords, AnimgraphSynonymMapsToAnimGraphInspect)
{
	using namespace SearchKeywordsTestHelpers;
	UNTEST_EXPECT_TRUE(KeywordsContain<ClaireonTool_AnimGraphInspect>(TEXT("animgraph")));
	co_return;
}

UNTEST_UNIT(Claireon, SearchKeywords, BpVariableSynonymMapsToAddVariable)
{
	using namespace SearchKeywordsTestHelpers;
	// 'bp variable' -> claireon.blueprint_graph_add_variable
	UNTEST_EXPECT_TRUE(KeywordsContain<ClaireonBlueprintGraphTool_AddVariable>(TEXT("bp")));
	UNTEST_EXPECT_TRUE(KeywordsContain<ClaireonBlueprintGraphTool_AddVariable>(TEXT("variable")));
	co_return;
}

UNTEST_UNIT(Claireon, SearchKeywords, BtTaskSynonymMapsToBehaviorTreeOpen)
{
	using namespace SearchKeywordsTestHelpers;
	// 'bt task' -> claireon.behaviortree_open (substituted from P3 _edit per registry)
	UNTEST_EXPECT_TRUE(KeywordsContain<ClaireonBehaviorTreeTool_Open>(TEXT("bt")));
	UNTEST_EXPECT_TRUE(KeywordsContain<ClaireonBehaviorTreeTool_Open>(TEXT("tree")));
	co_return;
}

UNTEST_UNIT(Claireon, SearchKeywords, AbbreviationSynonymsHonoredOnSessionEntryPoints)
{
	using namespace SearchKeywordsTestHelpers;
	// 'st' -> claireon.statetree_open
	UNTEST_EXPECT_TRUE(KeywordsContain<ClaireonStateTreeTool_Open>(TEXT("st")));
	// 'eqs' -> claireon.eqs_open
	UNTEST_EXPECT_TRUE(KeywordsContain<ClaireonEQSTool_Open>(TEXT("eqs")));
	// 'pcg' -> claireon.pcg_open
	UNTEST_EXPECT_TRUE(KeywordsContain<ClaireonPCGGraphTool_Open>(TEXT("pcg")));
	// 'umg' -> claireon.widgetbp_open
	UNTEST_EXPECT_TRUE(KeywordsContain<ClaireonWidgetBPTool_Open>(TEXT("umg")));
	// 'bb' -> claireon.blackboard_open
	UNTEST_EXPECT_TRUE(KeywordsContain<ClaireonBlackboardTool_Open>(TEXT("bb")));
	co_return;
}

#endif // WITH_UNTESTED
