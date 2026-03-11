// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Misc/AutomationTest.h"
#include "Tools/ClaireonTool_EditWidgetBP.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

namespace EditWidgetBPTestHelpers
{
	/** Build a JSON args object from a flat string->string map. */
	static TSharedPtr<FJsonObject> MakeArgs(const TMap<FString, FString>& Fields)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		for (const auto& Pair : Fields)
		{
			Args->SetStringField(Pair.Key, Pair.Value);
		}
		return Args;
	}

	/** Build a params sub-object from a flat string->string map. */
	static TSharedPtr<FJsonObject> MakeParams(const TMap<FString, FString>& Fields)
	{
		return MakeArgs(Fields);
	}

	/**
	 * Parse the session_id from a BuildStateResponse result.
	 * The response is pretty-printed JSON with a top-level "session_id" string field.
	 */
	static FString ParseSessionId(const FString& ResultText)
	{
		TSharedPtr<FJsonObject> Json;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultText);
		if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
		{
			return FString();
		}

		FString SessionId;
		Json->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	/** Parse result JSON into a FJsonObject. Returns invalid ptr on failure. */
	static TSharedPtr<FJsonObject> ParseJson(const FString& ResultText)
	{
		TSharedPtr<FJsonObject> Json;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResultText);
		FJsonSerializer::Deserialize(Reader, Json);
		return Json;
	}

	/**
	 * Execute create operation and return the session_id.
	 * Returns empty string on failure.
	 */
	static FString CreateWBP(ClaireonTool_EditWidgetBP& Tool,
		const FString& AssetPath,
		FAutomationTestBase* Test,
		const FString& RootWidgetClass = TEXT("CanvasPanel"))
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("create"));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), AssetPath);
		if (!RootWidgetClass.IsEmpty())
		{
			Params->SetStringField(TEXT("root_widget_class"), RootWidgetClass);
		}
		Args->SetObjectField(TEXT("params"), Params);

		auto Result = Tool.Execute(Args);
		if (Result.bIsError)
		{
			Test->AddError(FString::Printf(TEXT("Failed to create WBP '%s': %s"), *AssetPath, *Result.GetContentAsString()));
			return FString();
		}

		FString SessionId = ParseSessionId(Result.GetContentAsString());
		if (SessionId.IsEmpty())
		{
			Test->AddError(FString::Printf(TEXT("No session_id in create response for '%s'"), *AssetPath));
		}
		return SessionId;
	}

	/** Close a session (best-effort, ignores errors). */
	static void CloseSession(ClaireonTool_EditWidgetBP& Tool, const FString& SessionId)
	{
		if (SessionId.IsEmpty())
		{
			return;
		}

		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("close"));
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Tool.Execute(Args);
	}
} // namespace EditWidgetBPTestHelpers

// ===========================================================================
// Test 1: CreateAndBasicOps
// Create a WBP, add a TextBlock, set a property, compile, close.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_CreateAndBasicOps,
	"Claireon.EditWidgetBP.CreateAndBasicOps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_CreateAndBasicOps::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_EditTest");
	ClaireonTool_EditWidgetBP Tool;
	FString SessionId;

	// --- Step 1: Create the WBP ---
	SessionId = CreateWBP(Tool, AssetPath, this);
	if (SessionId.IsEmpty())
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("Created WBP, session: %s"), *SessionId));

	// --- Step 2: Add a TextBlock widget ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("add_widget"));
		Args->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
		Params->SetStringField(TEXT("widget_name"), TEXT("TestLabel"));
		Args->SetObjectField(TEXT("params"), Params);

		auto Result = Tool.Execute(Args);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add TextBlock: %s"), *Result.GetContentAsString()));
			CloseSession(Tool, SessionId);
			return false;
		}
		AddInfo(TEXT("Successfully added TextBlock 'TestLabel'"));
	}

	// --- Step 3: Set a property on the TextBlock ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("set_widget_property"));
		Args->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_name"), TEXT("TestLabel"));
		Params->SetStringField(TEXT("property_name"), TEXT("Text"));
		Params->SetStringField(TEXT("value"), TEXT("Hello"));
		Args->SetObjectField(TEXT("params"), Params);

		auto Result = Tool.Execute(Args);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to set Text property: %s"), *Result.GetContentAsString()));
			CloseSession(Tool, SessionId);
			return false;
		}
		AddInfo(TEXT("Successfully set Text property to 'Hello'"));
	}

	// --- Step 4: Compile ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("compile"));
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		auto Result = Tool.Execute(Args);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to compile: %s"), *Result.GetContentAsString()));
			CloseSession(Tool, SessionId);
			return false;
		}

		// Verify compile result contains success flag
		TSharedPtr<FJsonObject> CompileJson = ParseJson(Result.GetContentAsString());
		bool bSuccess = false;
		if (CompileJson.IsValid())
		{
			CompileJson->TryGetBoolField(TEXT("success"), bSuccess);
		}
		TestTrue("Compile should report success=true", bSuccess);
		AddInfo(TEXT("Successfully compiled WBP"));
	}

	// --- Step 5: Close ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("close"));
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		auto Result = Tool.Execute(Args);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to close session: %s"), *Result.GetContentAsString()));
			return false;
		}
		AddInfo(TEXT("Successfully closed session"));
	}

	return true;
}

// ===========================================================================
// Test 2: TreeManipulation
// Create WBP, build a hierarchy, move and remove widgets, verify tree state.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_TreeManipulation,
	"Claireon.EditWidgetBP.TreeManipulation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_TreeManipulation::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_TreeTest");
	ClaireonTool_EditWidgetBP Tool;

	// --- Create WBP with CanvasPanel root ---
	FString SessionId = CreateWBP(Tool, AssetPath, this, TEXT("CanvasPanel"));
	if (SessionId.IsEmpty())
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("Created WBP for tree manipulation, session: %s"), *SessionId));

	auto AddWidget = [&](const FString& WidgetClass, const FString& WidgetName, const FString& ParentName) -> bool
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("add_widget"));
		Args->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_class"), WidgetClass);
		Params->SetStringField(TEXT("widget_name"), WidgetName);
		if (!ParentName.IsEmpty())
		{
			Params->SetStringField(TEXT("parent_name"), ParentName);
		}
		Args->SetObjectField(TEXT("params"), Params);

		auto Result = Tool.Execute(Args);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add widget '%s' (class %s): %s"),
				*WidgetName, *WidgetClass, *Result.GetContentAsString()));
			return false;
		}
		return true;
	};

	// --- Build: CanvasPanel (root) -> VerticalBox -> TextBlock1, TextBlock2 ---

	// The CanvasPanel root was created automatically; add a VerticalBox under it
	if (!AddWidget(TEXT("VerticalBox"), TEXT("MyVerticalBox"), TEXT("CanvasPanel")))
	{
		CloseSession(Tool, SessionId);
		return false;
	}
	AddInfo(TEXT("Added VerticalBox under CanvasPanel"));

	if (!AddWidget(TEXT("TextBlock"), TEXT("TextBlock1"), TEXT("MyVerticalBox")))
	{
		CloseSession(Tool, SessionId);
		return false;
	}
	AddInfo(TEXT("Added TextBlock1 under VerticalBox"));

	if (!AddWidget(TEXT("TextBlock"), TEXT("TextBlock2"), TEXT("MyVerticalBox")))
	{
		CloseSession(Tool, SessionId);
		return false;
	}
	AddInfo(TEXT("Added TextBlock2 under VerticalBox"));

	// --- Move TextBlock1 to the root CanvasPanel ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("move_widget"));
		Args->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_name"), TEXT("TextBlock1"));
		Params->SetStringField(TEXT("new_parent_name"), TEXT("CanvasPanel"));
		Args->SetObjectField(TEXT("params"), Params);

		auto Result = Tool.Execute(Args);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to move TextBlock1: %s"), *Result.GetContentAsString()));
			CloseSession(Tool, SessionId);
			return false;
		}
		AddInfo(TEXT("Moved TextBlock1 to root CanvasPanel"));
	}

	// --- Remove TextBlock2 ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("remove_widget"));
		Args->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_name"), TEXT("TextBlock2"));
		Args->SetObjectField(TEXT("params"), Params);

		auto Result = Tool.Execute(Args);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to remove TextBlock2: %s"), *Result.GetContentAsString()));
			CloseSession(Tool, SessionId);
			return false;
		}
		AddInfo(TEXT("Removed TextBlock2"));
	}

	// --- Verify tree integrity via get_state ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("get_state"));
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		auto Result = Tool.Execute(Args);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to get_state: %s"), *Result.GetContentAsString()));
			CloseSession(Tool, SessionId);
			return false;
		}

		TSharedPtr<FJsonObject> StateJson = ParseJson(Result.GetContentAsString());
		TestTrue("get_state should return valid JSON", StateJson.IsValid());

		if (StateJson.IsValid())
		{
			const TSharedPtr<FJsonObject>* WidgetTreePtr = nullptr;
			TestTrue("State should contain 'widget_tree'", StateJson->TryGetObjectField(TEXT("widget_tree"), WidgetTreePtr));
		}
		AddInfo(TEXT("get_state returned valid tree state"));
	}

	CloseSession(Tool, SessionId);
	return true;
}

// ===========================================================================
// Test 3: SessionLifecycle
// Open creates a session, get_state works, close works, get_state after close fails.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_SessionLifecycle,
	"Claireon.EditWidgetBP.SessionLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_SessionLifecycle::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_LifecycleTest");
	ClaireonTool_EditWidgetBP Tool;

	// --- Step 1: Create opens a session ---
	FString SessionId = CreateWBP(Tool, AssetPath, this);
	if (SessionId.IsEmpty())
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("Session created successfully, id=%s"), *SessionId));

	// --- Step 2: get_state works on an open session ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("get_state"));
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		auto Result = Tool.Execute(Args);
		TestFalse("get_state on open session should not error", Result.bIsError);

		if (!Result.bIsError)
		{
			TSharedPtr<FJsonObject> StateJson = ParseJson(Result.GetContentAsString());
			TestTrue("get_state result should be valid JSON", StateJson.IsValid());

			if (StateJson.IsValid())
			{
				FString ReturnedSessionId;
				StateJson->TryGetStringField(TEXT("session_id"), ReturnedSessionId);
				TestEqual("Returned session_id should match", ReturnedSessionId, SessionId);
			}
			AddInfo(TEXT("get_state succeeded on open session"));
		}
	}

	// --- Step 3: close works ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("close"));
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		auto Result = Tool.Execute(Args);
		TestFalse("close should not error", Result.bIsError);

		if (!Result.bIsError)
		{
			TSharedPtr<FJsonObject> CloseJson = ParseJson(Result.GetContentAsString());
			bool bSuccess = false;
			if (CloseJson.IsValid())
			{
				CloseJson->TryGetBoolField(TEXT("success"), bSuccess);
			}
			TestTrue("close result should report success=true", bSuccess);
			AddInfo(TEXT("Session closed successfully"));
		}
	}

	// --- Step 4: get_state after close should fail ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("get_state"));
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		auto Result = Tool.Execute(Args);
		TestTrue("get_state on closed session should return error", Result.bIsError);

		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error when calling get_state on a closed session, but got success"));
			return false;
		}
		AddInfo(TEXT("Correctly rejected get_state on closed session"));
	}

	return true;
}

// ===========================================================================
// Test 4: ErrorHandling
// Invalid operation, missing session_id, invalid asset path, invalid widget class.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_ErrorHandling,
	"Claireon.EditWidgetBP.ErrorHandling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_ErrorHandling::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	ClaireonTool_EditWidgetBP Tool;

	// --- Test 1: Invalid operation ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("totally_invalid_operation"));
		Args->SetStringField(TEXT("session_id"), TEXT("fake-session-id"));
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		auto Result = Tool.Execute(Args);
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for invalid operation, but got success"));
			return false;
		}
		AddInfo(TEXT("Correctly rejected invalid operation"));
	}

	// --- Test 2: Missing session_id for a session-required operation ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("add_widget"));
		// No session_id set
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		auto Result = Tool.Execute(Args);
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for missing session_id"));
			return false;
		}
		TestTrue("Error message should mention 'session_id'",
			Result.GetContentAsString().Contains(TEXT("session_id")));
		AddInfo(TEXT("Correctly rejected missing session_id"));
	}

	// --- Test 3: Invalid asset path for create ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("create"));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		// Intentionally malformed path (not a /Game/ path, bare filename)
		Params->SetStringField(TEXT("asset_path"), TEXT("NotAValidUnrealPath"));
		Args->SetObjectField(TEXT("params"), Params);

		auto Result = Tool.Execute(Args);
		if (!Result.bIsError)
		{
			// Close the erroneously-opened session if somehow it succeeded
			FString SessionId = ParseSessionId(Result.GetContentAsString());
			if (!SessionId.IsEmpty())
			{
				CloseSession(Tool, SessionId);
			}
			AddError(TEXT("Expected error for invalid asset path"));
			return false;
		}
		AddInfo(TEXT("Correctly rejected invalid asset path"));
	}

	// --- Test 4: Invalid widget class for add_widget ---
	{
		// First create a valid session
		const FString AssetPath = TEXT("/Game/__MCPTests/WBP_ErrorTest");
		FString SessionId = CreateWBP(Tool, AssetPath, this);
		if (SessionId.IsEmpty())
		{
			return false;
		}

		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("add_widget"));
		Args->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_class"), TEXT("ThisClassDoesNotExistAnywhere"));
		Params->SetStringField(TEXT("widget_name"), TEXT("ShouldNeverExist"));
		Args->SetObjectField(TEXT("params"), Params);

		auto Result = Tool.Execute(Args);
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for invalid widget class"));
			CloseSession(Tool, SessionId);
			return false;
		}
		AddInfo(FString::Printf(TEXT("Correctly rejected invalid widget class with message: %s"),
			*Result.GetContentAsString()));

		CloseSession(Tool, SessionId);
	}

	return true;
}

// ===========================================================================
// Test 5: AnimationLifecycle
// Create, rename, duplicate, set_property, delete animations.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_AnimationLifecycle,
	"Claireon.EditWidgetBP.AnimationLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_AnimationLifecycle::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_AnimLifecycleTest");
	ClaireonTool_EditWidgetBP Tool;

	FString SessionId = CreateWBP(Tool, AssetPath, this);
	if (SessionId.IsEmpty())
	{
		return false;
	}

	auto Exec = [&](const FString& Op, const TSharedPtr<FJsonObject>& Params) -> IClaireonTool::FToolResult
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), Op);
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), Params);
		return Tool.Execute(Args);
	};

	// --- Step 1: create_animation "FadeIn" ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
		Params->SetNumberField(TEXT("duration"), 2.0);

		auto Result = Exec(TEXT("create_animation"), Params);
		TestFalse("create_animation FadeIn should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			FString Name;
			Json->TryGetStringField(TEXT("name"), Name);
			TestEqual("Animation name should be FadeIn", Name, TEXT("FadeIn"));
		}
	}

	// --- Step 2: create_animation "SlideOut" ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("SlideOut"));
		Params->SetNumberField(TEXT("duration"), 3.0);

		auto Result = Exec(TEXT("create_animation"), Params);
		TestFalse("create_animation SlideOut should succeed", Result.bIsError);
	}

	// --- Step 3: list_animations — verify count == 2 ---
	{
		auto Result = Exec(TEXT("list_animations"), MakeShared<FJsonObject>());
		TestFalse("list_animations should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			double Count = 0;
			Json->TryGetNumberField(TEXT("count"), Count);
			TestEqual("Should have 2 animations", static_cast<int32>(Count), 2);
		}
	}

	// --- Step 4: rename_animation "FadeIn" → "FadeInNew" ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
		Params->SetStringField(TEXT("new_name"), TEXT("FadeInNew"));

		auto Result = Exec(TEXT("rename_animation"), Params);
		TestFalse("rename_animation should succeed", Result.bIsError);
	}

	// --- Step 5: set_animation_property duration to 4.0 ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeInNew"));
		Params->SetNumberField(TEXT("duration"), 4.0);

		auto Result = Exec(TEXT("set_animation_property"), Params);
		TestFalse("set_animation_property should succeed", Result.bIsError);
	}

	// --- Step 6: duplicate_animation "FadeInNew" ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeInNew"));

		auto Result = Exec(TEXT("duplicate_animation"), Params);
		TestFalse("duplicate_animation should succeed", Result.bIsError);
	}

	// --- Step 7: delete_animation "SlideOut" ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("SlideOut"));

		auto Result = Exec(TEXT("delete_animation"), Params);
		TestFalse("delete_animation should succeed", Result.bIsError);
	}

	// --- Step 8: list_animations — verify count == 2 (FadeInNew + copy) ---
	{
		auto Result = Exec(TEXT("list_animations"), MakeShared<FJsonObject>());
		TestFalse("list_animations should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			double Count = 0;
			Json->TryGetNumberField(TEXT("count"), Count);
			TestEqual("Should have 2 animations after delete+duplicate", static_cast<int32>(Count), 2);
		}
	}

	// --- Step 9: get_animation_details ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeInNew"));

		auto Result = Exec(TEXT("get_animation_details"), Params);
		TestFalse("get_animation_details should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			double DisplayRate = 0;
			Json->TryGetNumberField(TEXT("display_rate"), DisplayRate);
			TestEqual("Display rate should be 20", static_cast<int32>(DisplayRate), 20);
		}
	}

	// --- Step 10: compile ---
	{
		auto Result = Exec(TEXT("compile"), MakeShared<FJsonObject>());
		TestFalse("compile should succeed", Result.bIsError);
	}

	CloseSession(Tool, SessionId);
	return true;
}

// ===========================================================================
// Test 6: AnimationBindingTracksKeyframes
// Create animation, bind widget, add track, add keyframes, remove keyframe.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_AnimationBindingTracksKeyframes,
	"Claireon.EditWidgetBP.AnimationBindingTracksKeyframes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_AnimationBindingTracksKeyframes::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_AnimTrackTest");
	ClaireonTool_EditWidgetBP Tool;

	FString SessionId = CreateWBP(Tool, AssetPath, this);
	if (SessionId.IsEmpty())
	{
		return false;
	}

	auto Exec = [&](const FString& Op, const TSharedPtr<FJsonObject>& Params) -> IClaireonTool::FToolResult
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), Op);
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), Params);
		return Tool.Execute(Args);
	};

	// --- Step 1: Add a TextBlock widget ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
		Params->SetStringField(TEXT("widget_name"), TEXT("FadeLabel"));

		auto Result = Exec(TEXT("add_widget"), Params);
		TestFalse("add_widget should succeed", Result.bIsError);
	}

	// --- Step 2: Create animation ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetNumberField(TEXT("duration"), 2.0);

		auto Result = Exec(TEXT("create_animation"), Params);
		TestFalse("create_animation should succeed", Result.bIsError);
	}

	// --- Step 3: Bind widget to animation ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetStringField(TEXT("widget_name"), TEXT("FadeLabel"));

		auto Result = Exec(TEXT("add_animation_binding"), Params);
		TestFalse("add_animation_binding should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			FString Guid;
			Json->TryGetStringField(TEXT("widget_guid"), Guid);
			TestFalse("widget_guid should not be empty", Guid.IsEmpty());
		}
	}

	// --- Step 4: Add RenderOpacity track ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetStringField(TEXT("widget_name"), TEXT("FadeLabel"));
		Params->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));

		auto Result = Exec(TEXT("add_animation_track"), Params);
		TestFalse("add_animation_track should succeed", Result.bIsError);
	}

	// --- Step 5: Add keyframe at t=0 (opacity=0) ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetStringField(TEXT("widget_name"), TEXT("FadeLabel"));
		Params->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));
		Params->SetNumberField(TEXT("time"), 0.0);
		Params->SetNumberField(TEXT("value"), 0.0);
		Params->SetStringField(TEXT("interpolation"), TEXT("linear"));

		auto Result = Exec(TEXT("add_animation_keyframe"), Params);
		TestFalse("add_animation_keyframe at t=0 should succeed", Result.bIsError);
	}

	// --- Step 6: Add keyframe at t=2 (opacity=1) ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetStringField(TEXT("widget_name"), TEXT("FadeLabel"));
		Params->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));
		Params->SetNumberField(TEXT("time"), 2.0);
		Params->SetNumberField(TEXT("value"), 1.0);

		auto Result = Exec(TEXT("add_animation_keyframe"), Params);
		TestFalse("add_animation_keyframe at t=2 should succeed", Result.bIsError);
	}

	// --- Step 7: Verify via get_animation_details ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));

		auto Result = Exec(TEXT("get_animation_details"), Params);
		TestFalse("get_animation_details should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
			if (Json->TryGetArrayField(TEXT("bindings"), Bindings))
			{
				TestTrue("Should have at least 1 binding", Bindings->Num() >= 1);
			}
		}
	}

	// --- Step 8: Remove keyframe at t=0 ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetStringField(TEXT("widget_name"), TEXT("FadeLabel"));
		Params->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));
		Params->SetNumberField(TEXT("time"), 0.0);

		auto Result = Exec(TEXT("remove_animation_keyframe"), Params);
		TestFalse("remove_animation_keyframe should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			double RemovedCount = 0;
			Json->TryGetNumberField(TEXT("removed_count"), RemovedCount);
			TestTrue("Should have removed at least 1 key", RemovedCount >= 1.0);
		}
	}

	// --- Step 9: Error — add track for unbound widget ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetStringField(TEXT("widget_name"), TEXT("NonExistentWidget"));
		Params->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));

		auto Result = Exec(TEXT("add_animation_track"), Params);
		TestTrue("add_animation_track for unbound widget should error", Result.bIsError);
	}

	CloseSession(Tool, SessionId);
	return true;
}
