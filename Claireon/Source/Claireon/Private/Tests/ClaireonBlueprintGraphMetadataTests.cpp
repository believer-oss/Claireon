// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for blueprint_graph_* metadata overrides (P1, Stage 007). Asserts
// that each of the 10 hot-path tools returns rich GetFullDescription /
// GetExampleUsage, that the 4 spec'd tools return GetParameterTooltips,
// and that the workflow rules from the per-tool authoring guidance
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
UNTEST_UNIT(Claireon, BlueprintGraphMetadata, AllTenToolsHaveRichMetadata)
{
	using namespace BPGraphMetadataTestHelpers;
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_Open>(TEXT("blueprint_graph_open")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_AddNode>(TEXT("blueprint_graph_add_node")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_ConnectPins>(TEXT("blueprint_graph_connect_pins")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_SetPinValue>(TEXT("blueprint_graph_set_pin_value")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_SelectPin>(TEXT("blueprint_graph_select_pin")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_AddVariable>(TEXT("blueprint_graph_add_variable")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_Save>(TEXT("blueprint_graph_save")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_Format>(TEXT("blueprint_graph_format")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_Compile>(TEXT("blueprint_graph_compile")));
	UNTEST_EXPECT_TRUE(ValidateMetadataLengthsAndContent<ClaireonBlueprintGraphTool_Close>(TEXT("blueprint_graph_close")));
	co_return;
}

// ---------------------------------------------------------------------------
// 3: auto_connect_from_cursor token surfaces in add_node full description.
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, BlueprintGraphMetadata, AddNodeFullDescriptionMentionsAutoConnect)
{
	ClaireonBlueprintGraphTool_AddNode Tool;
	const FString Full = Tool.GetFullDescription();
	UNTEST_EXPECT_TRUE(Full.Contains(TEXT("auto_connect_from_cursor")));
	co_return;
}

// ---------------------------------------------------------------------------
// 4: format-tool gotcha surfaces -- the in-session formatter mentions the
// standalone twin blueprint_format_graph.
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, BlueprintGraphMetadata, FormatFullDescriptionMentionsStandaloneTwin)
{
	ClaireonBlueprintGraphTool_Format Tool;
	const FString Full = Tool.GetFullDescription();
	UNTEST_EXPECT_TRUE(Full.Contains(TEXT("blueprint_format_graph")));
	co_return;
}

// ---------------------------------------------------------------------------
// 5: ParameterTooltips coverage on the 4 spec'd tools (add_node,
// connect_pins, set_pin_value, add_variable). Each tool's tooltip object
// must contain entries for the named required parameters.
// ---------------------------------------------------------------------------
UNTEST_UNIT(Claireon, BlueprintGraphMetadata, AddNodeParameterTooltipsCoverRequired)
{
	ClaireonBlueprintGraphTool_AddNode Tool;
	TSharedPtr<FJsonObject> T = Tool.GetParameterTooltips();
	UNTEST_ASSERT_TRUE(T.IsValid());
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("session_id")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("node_class")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("auto_connect_from_cursor")));
	co_return;
}

UNTEST_UNIT(Claireon, BlueprintGraphMetadata, ConnectPinsParameterTooltipsCoverRequired)
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

UNTEST_UNIT(Claireon, BlueprintGraphMetadata, SetPinValueParameterTooltipsCoverRequired)
{
	ClaireonBlueprintGraphTool_SetPinValue Tool;
	TSharedPtr<FJsonObject> T = Tool.GetParameterTooltips();
	UNTEST_ASSERT_TRUE(T.IsValid());
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("node")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("pin")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("value")));
	co_return;
}

UNTEST_UNIT(Claireon, BlueprintGraphMetadata, AddVariableParameterTooltipsCoverRequired)
{
	ClaireonBlueprintGraphTool_AddVariable Tool;
	TSharedPtr<FJsonObject> T = Tool.GetParameterTooltips();
	UNTEST_ASSERT_TRUE(T.IsValid());
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("name")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("variable_type")));
	UNTEST_EXPECT_TRUE(T->HasField(TEXT("variable_type_spec")));
	co_return;
}

#endif // WITH_UNTESTED
