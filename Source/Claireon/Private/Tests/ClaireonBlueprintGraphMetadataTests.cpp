// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for bp_* metadata overrides. Asserts
// that each of the 10 hot-path tools returns rich GetFullDescription /
// GetExampleUsage, that the 4 spec'd tools return GetParameterTooltips,
// and that the workflow rules from .claude/areas/blueprint-editing.md
// surface in the literal text.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
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
#include "Dom/JsonObject.h"

namespace BPGraphMetadataTestHelpers
{
	template <typename ToolT>
	bool ValidateMetadataLengthsAndContent(const TCHAR* ExpectedNameSubstr)
	{
		ToolT Tool;
		const FString Name = Tool.GetName();
		const FString FullDesc = Tool.GetFullDescription();
		const FString Example = Tool.GetExampleUsage();
		const FString StdDesc = Tool.GetDescription();

		if (ExpectedNameSubstr && !Name.Contains(ExpectedNameSubstr))
		{
			UE_LOG(LogTemp, Error, TEXT("[BPGraphMetadata] Tool name '%s' missing expected substring '%s'"), *Name, ExpectedNameSubstr);
			return false;
		}
		if (FullDesc.Len() < 200)
		{
			UE_LOG(LogTemp, Error, TEXT("[BPGraphMetadata] Tool '%s' GetFullDescription too short (%d chars; expected >= 200)"), *Name, FullDesc.Len());
			return false;
		}
		if (Example.IsEmpty())
		{
			UE_LOG(LogTemp, Error, TEXT("[BPGraphMetadata] Tool '%s' has empty GetExampleUsage"), *Name);
			return false;
		}
		// Standard description must stay in [80, 400].
		if (StdDesc.Len() < 80 || StdDesc.Len() > 400)
		{
			UE_LOG(LogTemp, Error, TEXT("[BPGraphMetadata] Tool '%s' standard GetDescription out of [80,400] range (%d chars)"), *Name, StdDesc.Len());
			return false;
		}
		return true;
	}
}

// ---------------------------------------------------------------------------
// 1+2+6: Length assertions + non-empty example + standard description range
// for all 10 P1 tools.
// ---------------------------------------------------------------------------
// Budget: the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs),
// which is not a deliberate perf assertion. This test sweeps the fully-populated
// ~717-tool registry, so give it real headroom instead of restoring the default.
UNTEST_UNIT_OPTS(Claireon, BlueprintGraphMetadata, AllTenToolsHaveRichMetadata, UNTEST_TIMEOUTMS(30000))
{
	using namespace BPGraphMetadataTestHelpers;
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_Open>(TEXT("bp_open")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_AddNode>(TEXT("bp_add_node")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_ConnectPins>(TEXT("bp_connect_pins")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_SetPinValue>(TEXT("bp_set_pin_value")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_SelectPin>(TEXT("bp_select_pin")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_AddVariable>(TEXT("bp_add_variable")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_Save>(TEXT("bp_save")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_Format>(TEXT("bp_format")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_Compile>(TEXT("bp_compile")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_Close>(TEXT("bp_close")));
	co_return;
}

// ---------------------------------------------------------------------------
// 3: auto_connect_from_cursor token surfaces in add_node full description.
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, BlueprintGraphMetadata, AddNodeFullDescriptionMentionsAutoConnect, UNTEST_TIMEOUTMS(10000))
{
	ClaireonBlueprintGraphTool_AddNode Tool;
	const FString Full = Tool.GetFullDescription();
	UNTEST_EXPECT_TRUE(Full.Contains(TEXT("auto_connect_from_cursor")));
	co_return;
}

// ---------------------------------------------------------------------------
// 4: format-tool description mentions the asset_path auto-open path. After
// the namespace collapse, bp_format accepts either session_id (in-session)
// or asset_path (auto-opens a transient session, formats, closes); the
// description must surface that ergonomic.
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, BlueprintGraphMetadata, FormatFullDescriptionMentionsAutoOpenPath, UNTEST_TIMEOUTMS(10000))
{
	ClaireonBlueprintGraphTool_Format Tool;
	const FString Full = Tool.GetFullDescription();
	UNTEST_EXPECT_TRUE(Full.Contains(TEXT("asset_path")));
	UNTEST_EXPECT_TRUE(Full.Contains(TEXT("auto-opens")));
	co_return;
}

// ---------------------------------------------------------------------------
// 5: ParameterTooltips coverage on the 4 spec'd tools (add_node,
// connect_pins, set_pin_value, add_variable). Each tool's tooltip object
// must contain entries for the named required parameters.
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, BlueprintGraphMetadata, AddNodeParameterTooltipsCoverRequired, UNTEST_TIMEOUTMS(10000))
{
	ClaireonBlueprintGraphTool_AddNode Tool;
	TSharedPtr<FJsonObject> T = Tool.GetParameterTooltips();
	UNTEST_ASSERT_TRUE(T.IsValid());
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("session_id")));
	// node_type is the tool's only REQUIRED argument, so a test named
	// "CoverRequired" must assert it; it previously asserted only node_class,
	// which the schema scopes to CallFunction overrides.
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("node_type")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("node_class")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("auto_connect_from_cursor")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintGraphMetadata, ConnectPinsParameterTooltipsCoverRequired, UNTEST_TIMEOUTMS(10000))
{
	ClaireonBlueprintGraphTool_ConnectPins Tool;
	TSharedPtr<FJsonObject> T = Tool.GetParameterTooltips();
	UNTEST_ASSERT_TRUE(T.IsValid());
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("from_node")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("from_pin")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("to_node")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("to_pin")));
	co_return;
}

// These two asserted the tooltip keys "node"/"pin"/"name", which no schema declares
// and no Execute() reads -- bp_set_pin_value requires node_guid/pin_name and
// bp_add_variable requires variable_name. So they pinned false documentation: an agent
// following those tooltips gets "Missing required field: node_guid". Contrast
// bp_connect_pins, which really does accept from_*/to_* and therefore DECLARES those
// aliases in its schema (the pattern to follow when an alias is intended); here the
// short names were never accepted, so the tooltips were renamed rather than turned
// into new aliases. Assert the canonical required names, which is what these tests'
// "CoverRequired" names always claimed to check.
// Cross-checked by
// BPFeedbackSessionContract.ToolMetadata_TooltipKeysAreSchemaProperties, which now
// audits the whole registry for exactly this class of drift.
UNTEST_UNIT_OPTS(Claireon, BlueprintGraphMetadata, SetPinValueParameterTooltipsCoverRequired, UNTEST_TIMEOUTMS(10000))
{
	ClaireonBlueprintGraphTool_SetPinValue Tool;
	TSharedPtr<FJsonObject> T = Tool.GetParameterTooltips();
	UNTEST_ASSERT_TRUE(T.IsValid());
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("node_guid")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("pin_name")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("value")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintGraphMetadata, AddVariableParameterTooltipsCoverRequired, UNTEST_TIMEOUTMS(10000))
{
	ClaireonBlueprintGraphTool_AddVariable Tool;
	TSharedPtr<FJsonObject> T = Tool.GetParameterTooltips();
	UNTEST_ASSERT_TRUE(T.IsValid());
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("variable_name")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("variable_type")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("variable_type_spec")));
	co_return;
}

// ===========================================================================
// bp_add_node GetPatterns() is non-empty, ASCII-clean (no em-dash), and
// the migrated content no longer lives in GetFullDescription().
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, BlueprintGraphMetadata, AddNodePatternsNonEmptyAndAscii, UNTEST_TIMEOUTMS(10000))
{
	ClaireonBlueprintGraphTool_AddNode Tool;
	const FString Patterns = Tool.GetPatterns();
	UNTEST_EXPECT_TRUE(!Patterns.IsEmpty());
	UNTEST_EXPECT_TRUE(Patterns.Contains(TEXT("## Common pitfalls")));
	UNTEST_EXPECT_TRUE(Patterns.Contains(TEXT("## See also")));
	// ASCII-clean: reject em/en dashes and the non-breaking space code unit.
	for (int32 I = 0; I < Patterns.Len(); ++I)
	{
		const TCHAR C = Patterns[I];
		UNTEST_EXPECT_FALSE(C == TCHAR(0x2013)); // en dash
		UNTEST_EXPECT_FALSE(C == TCHAR(0x2014)); // em dash
		UNTEST_EXPECT_FALSE(C == TCHAR(0x00A0)); // non-breaking space
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintGraphMetadata, AddNodeFullDescriptionDoesNotMentionPerNodeCycle, UNTEST_TIMEOUTMS(10000))
{
	ClaireonBlueprintGraphTool_AddNode Tool;
	const FString Full = Tool.GetFullDescription();
	UNTEST_EXPECT_FALSE(Full.Contains(TEXT("per-node cycle")));
	co_return;
}

#endif // WITH_UNTESTED
