// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Spec tests for bp_get_generated_class.
// Smoke-level: surface registration + required-field validation.
// Asset-backed validation is exercised manually via the editor.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonTool_BlueprintGetGeneratedClass.h"
#include "Dom/JsonObject.h"

UNTEST_UNIT_OPTS(Claireon, BlueprintGetGeneratedClass, ToolSurface, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_BlueprintGetGeneratedClass Tool;
	UNTEST_ASSERT_STREQ(*Tool.GetName(), TEXT("bp_get_generated_class"));
	UNTEST_ASSERT_TRUE(!Tool.GetDescription().IsEmpty());
	const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintGetGeneratedClass, MissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_BlueprintGetGeneratedClass Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.ErrorMessage.Contains(TEXT("asset_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintGetGeneratedClass, BogusAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_BlueprintGetGeneratedClass Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotExist.DoesNotExist"));
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

#endif // WITH_UNTESTED
