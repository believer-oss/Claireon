// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Spec tests for pie_register_actor.
// Smoke-level: verify tool-surface registration and outside-PIE error path.
// Asset-backed registration is exercised manually via the editor.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonTool_PIERegisterActor.h"
#include "Dom/JsonObject.h"

UNTEST_UNIT_OPTS(Claireon, PIERegisterActor, ToolSurface, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_PIERegisterActor Tool;
	UNTEST_ASSERT_STREQ(*Tool.GetName(), TEXT("pie_register_actor"));
	UNTEST_ASSERT_TRUE(!Tool.GetDescription().IsEmpty());
	const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PIERegisterActor, MissingActorPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_PIERegisterActor Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.ErrorMessage.Contains(TEXT("actor_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PIERegisterActor, OutsidePIE, UNTEST_TIMEOUTMS(5000))
{
	// Spec tests run outside PIE; the tool must surface a clean "PIE is not running"
	// rather than crash or return success.
	ClaireonTool_PIERegisterActor Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("actor_path"), TEXT("/Game/Maps/UEDPIE_0_DoesNotExist.DoesNotExist:PersistentLevel.Nobody_C_0"));
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	co_return;
}

#endif // WITH_UNTESTED
