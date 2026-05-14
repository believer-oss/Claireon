// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Spec tests for claireon.statetree_get_schema (#0000 / F3).
// Smoke-level: verify the tool surface registers cleanly. Asset-backed
// integration tests are exercised manually via the editor.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonTool_StateTreeGetSchema.h"
#include "Dom/JsonObject.h"

UNTEST_UNIT_OPTS(Claireon, StateTreeGetSchema, ToolSurface, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_StateTreeGetSchema Tool;
	UNTEST_ASSERT_STREQ(*Tool.GetName(), TEXT("claireon.statetree_get_schema"));
	UNTEST_ASSERT_TRUE(!Tool.GetDescription().IsEmpty());
	const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, StateTreeGetSchema, MissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_StateTreeGetSchema Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.ErrorMessage.Contains(TEXT("Missing required field: asset_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, StateTreeGetSchema, BogusAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_StateTreeGetSchema Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotExist.DoesNotExist"));
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

#endif // WITH_UNTESTED
