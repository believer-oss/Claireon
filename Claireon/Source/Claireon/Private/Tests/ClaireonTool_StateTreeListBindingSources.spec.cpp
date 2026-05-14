// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Spec tests for statetree_list_binding_sources (#0000 / F5).
// Surface-level: real walks require a loaded asset, exercised manually.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonTool_StateTreeListBindingSources.h"
#include "Dom/JsonObject.h"

UNTEST_UNIT_OPTS(Claireon, StateTreeListBindingSources, ToolSurface, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_StateTreeListBindingSources Tool;
	UNTEST_ASSERT_STREQ(*Tool.GetName(), TEXT("statetree_list_binding_sources"));
	UNTEST_ASSERT_TRUE(!Tool.GetDescription().IsEmpty());
	const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, StateTreeListBindingSources, MissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_StateTreeListBindingSources Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.ErrorMessage.Contains(TEXT("Missing required field: asset_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, StateTreeListBindingSources, InvalidStateGuid, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_StateTreeListBindingSources Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/AnyAsset.AnyAsset"));
	Args->SetStringField(TEXT("state_id"), TEXT("not-a-guid"));
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.ErrorMessage.Contains(TEXT("Invalid state_id format")));
	co_return;
}

#endif // WITH_UNTESTED
