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
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"

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

// ---------------------------------------------------------------------------
// P2-8: class_filter is an is-a test (include_subclasses default true), with
// the legacy substring semantics surviving only as a DISCLOSED fallback for
// names that do not resolve to a class.
// ---------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, ListActors, ClassFilterIsATest, UNTEST_TIMEOUTMS(30000))
{
	if (!IsValid(GEditor) || !IsValid(GEditor->GetEditorWorldContext().World()))
	{
		UE_LOG(LogTemp, Log, TEXT("ClassFilterIsATest: no editor world in this harness, skipping."));
		co_return;
	}
	UWorld* World = GEditor->GetEditorWorldContext().World();

	AActor* SubclassActor = World->SpawnActor<AStaticMeshActor>();
	AActor* PlainActor = World->SpawnActor<AActor>();
	UNTEST_ASSERT_PTR(SubclassActor);
	UNTEST_ASSERT_PTR(PlainActor);
	const FString SubclassName = SubclassActor->GetName();
	const FString PlainName = PlainActor->GetName();

	ClaireonTool_ListActors Tool;

	auto ResultNamesContain = [](const IClaireonTool::FToolResult& Result, const FString& Name) -> bool
	{
		const TArray<TSharedPtr<FJsonValue>>* Actors = nullptr;
		if (Result.bIsError || !Result.Data.IsValid()
			|| !Result.Data->TryGetArrayField(TEXT("actors"), Actors))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Val : *Actors)
		{
			const TSharedPtr<FJsonObject> Obj = Val->AsObject();
			if (Obj.IsValid() && Obj->GetStringField(TEXT("name")) == Name)
			{
				return true;
			}
		}
		return false;
	};

	// Default (is-a, subclasses included): filter 'Actor' finds both.
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("class_filter"), TEXT("Actor"));
		Args->SetStringField(TEXT("world_context"), TEXT("editor"));
		const IClaireonTool::FToolResult R = Tool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
		UNTEST_EXPECT_TRUE(ResultNamesContain(R, SubclassName));
		UNTEST_EXPECT_TRUE(ResultNamesContain(R, PlainName));
	}

	// include_subclasses=false: exact class only.
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("class_filter"), TEXT("Actor"));
		Args->SetBoolField(TEXT("include_subclasses"), false);
		Args->SetStringField(TEXT("world_context"), TEXT("editor"));
		const IClaireonTool::FToolResult R = Tool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
		UNTEST_EXPECT_FALSE(ResultNamesContain(R, SubclassName));
		UNTEST_EXPECT_TRUE(ResultNamesContain(R, PlainName));
	}

	// The old defect shape: a BP-subclass-missing filter. StaticMeshActor
	// finds the SMA but never the plain actor under is-a semantics.
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("class_filter"), TEXT("StaticMeshActor"));
		Args->SetStringField(TEXT("world_context"), TEXT("editor"));
		const IClaireonTool::FToolResult R = Tool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
		UNTEST_EXPECT_TRUE(ResultNamesContain(R, SubclassName));
		UNTEST_EXPECT_FALSE(ResultNamesContain(R, PlainName));
	}

	// Unresolvable name: succeeds via the substring fallback and SAYS SO.
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("class_filter"), TEXT("TotallyBogusClass123"));
		Args->SetStringField(TEXT("world_context"), TEXT("editor"));
		const IClaireonTool::FToolResult R = Tool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
		bool bDisclosed = false;
		for (const FString& W : R.Warnings)
		{
			if (W.Contains(TEXT("falling back")))
			{
				bDisclosed = true;
				break;
			}
		}
		UNTEST_EXPECT_TRUE(bDisclosed);
	}

	World->DestroyActor(SubclassActor);
	World->DestroyActor(PlainActor);
	co_return;
}

#endif // WITH_UNTESTED
