// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Spec tests for statetree_create.
// Smoke-level: verify the tool surface registers cleanly and required-field
// validation errors fire on missing inputs. Asset-backed creation is
// exercised manually via the editor.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonStateTreeTool_Create.h"
#include "Dom/JsonObject.h"

UNTEST_UNIT_OPTS(Claireon, StateTreeCreate, ToolSurface, UNTEST_TIMEOUTMS(5000))
{
	ClaireonStateTreeTool_Create Tool;
	UNTEST_ASSERT_STREQ(*Tool.GetName(), TEXT("statetree_create"));
	UNTEST_ASSERT_TRUE(!Tool.GetDescription().IsEmpty());
	const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, StateTreeCreate, MissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonStateTreeTool_Create Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("schema_class_path"), TEXT("/Script/StateTreeModule.StateTreeSchema"));
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.ErrorMessage.Contains(TEXT("asset_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, StateTreeCreate, MissingSchemaClassPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonStateTreeTool_Create Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/AI/ST_DoesNotMatter"));
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.ErrorMessage.Contains(TEXT("schema_class_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, StateTreeCreate, BogusSchemaClass, UNTEST_TIMEOUTMS(5000))
{
	ClaireonStateTreeTool_Create Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/AI/ST_BogusSchema_DoesNotMatter"));
	Args->SetStringField(TEXT("schema_class_path"), TEXT("/Script/Engine.NotAStateTreeSchemaXYZ"));
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

#endif // WITH_UNTESTED
