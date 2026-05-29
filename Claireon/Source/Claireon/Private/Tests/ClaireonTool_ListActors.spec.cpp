// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// Spec tests for level_list_actors.
// Smoke-level: tool surface, schema shape, and outside-PIE default path.
// Full PIE routing verified manually via the editor.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonTool_ListActors.h"
#include "Dom/JsonObject.h"

UNTEST_UNIT_OPTS(Claireon, ListActors, ToolSurface, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_ListActors Tool;
	UNTEST_ASSERT_STREQ(*Tool.GetName(), TEXT("level_list_actors"));
	UNTEST_ASSERT_TRUE(!Tool.GetDescription().IsEmpty());
	const TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_TRUE(Schema.IsValid());
	// Schema must expose world_context parameter
	const TSharedPtr<FJsonObject>* Props = nullptr;
	UNTEST_ASSERT_TRUE(Schema->TryGetObjectField(TEXT("properties"), Props) && Props != nullptr);
	UNTEST_ASSERT_TRUE((*Props)->HasField(TEXT("world_context")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ListActors, PIEModeRequiresPIE, UNTEST_TIMEOUTMS(5000))
{
	// Spec tests run outside PIE; world_context='pie' must surface a clean error.
	ClaireonTool_ListActors Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("world_context"), TEXT("pie"));
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.ErrorMessage.Contains(TEXT("PIE")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ListActors, AutoModeOutsidePIE, UNTEST_TIMEOUTMS(5000))
{
	// Outside PIE, auto falls back to editor world. If no map is loaded the
	// tool should return an error (not crash); if a map is loaded it succeeds.
	// We only verify the no-crash / shape contract here; the map-loaded
	// success case is covered manually via the editor.
	ClaireonTool_ListActors Tool;
	const TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	// world_context omitted -> 'auto'
	const IClaireonTool::FToolResult Result = Tool.Execute(Args);
	// Either succeeds (map loaded in test context) or errors with a
	// message referencing "world" or "map" -- either is acceptable; crash is not.
	if (Result.bIsError)
	{
		const bool bMentionsWorld =
			Result.ErrorMessage.Contains(TEXT("world"), ESearchCase::IgnoreCase) ||
			Result.ErrorMessage.Contains(TEXT("map"), ESearchCase::IgnoreCase);
		UNTEST_ASSERT_TRUE(bMentionsWorld);
	}
	else
	{
		UNTEST_ASSERT_TRUE(Result.Data.IsValid());
		UNTEST_ASSERT_TRUE(Result.Data->HasField(TEXT("actors")));
		UNTEST_ASSERT_TRUE(Result.Data->HasField(TEXT("world_context")));
		FString UsedCtx;
		UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("world_context"), UsedCtx));
		UNTEST_EXPECT_STREQ(*UsedCtx, TEXT("editor"));
	}
	co_return;
}

#endif // WITH_UNTESTED
