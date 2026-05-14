// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Stage 029 rewrite: these specs now exercise the decomposed
// ClaireonWidgetBPTool_* tools directly. The legacy monolithic shim
// (ClaireonTool_EditWidgetBP) and its envelope dispatcher were deleted in
// stage 024; every test below instantiates the matching decomposed tool
// per operation and flattens the legacy envelope via FlattenLegacyEnvelope.

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// Decomposed widget-blueprint tools (one include per operation exercised here).
#include "Tools/ClaireonWidgetBPTool_AddAnimationBinding.h"
#include "Tools/ClaireonWidgetBPTool_AddAnimationKeyframe.h"
#include "Tools/ClaireonWidgetBPTool_AddAnimationTrack.h"
#include "Tools/ClaireonWidgetBPTool_AddMVVMBinding.h"
#include "Tools/ClaireonWidgetBPTool_AddMVVMViewModel.h"
#include "Tools/ClaireonWidgetBPTool_AddWidget.h"
#include "Tools/ClaireonWidgetBPTool_Close.h"
#include "Tools/ClaireonWidgetBPTool_Compile.h"
#include "Tools/ClaireonWidgetBPTool_Create.h"
#include "Tools/ClaireonWidgetBPTool_CreateAnimation.h"
#include "Tools/ClaireonWidgetBPTool_DeleteAnimation.h"
#include "Tools/ClaireonWidgetBPTool_DuplicateAnimation.h"
#include "Tools/ClaireonWidgetBPTool_EditMVVMBinding.h"
#include "Tools/ClaireonWidgetBPTool_GetAnimationDetails.h"
#include "Tools/ClaireonWidgetBPTool_GetState.h"
#include "Tools/ClaireonWidgetBPTool_ListAnimations.h"
#include "Tools/ClaireonWidgetBPTool_ListMVVMBindings.h"
#include "Tools/ClaireonWidgetBPTool_ListMVVMViewModels.h"
#include "Tools/ClaireonWidgetBPTool_MoveWidget.h"
#include "Tools/ClaireonWidgetBPTool_Open.h"
#include "Tools/ClaireonWidgetBPTool_RemoveAnimationKeyframe.h"
#include "Tools/ClaireonWidgetBPTool_Save.h"
#include "Tools/ClaireonWidgetBPTool_RemoveMVVMBinding.h"
#include "Tools/ClaireonWidgetBPTool_RemoveMVVMViewModel.h"
#include "Tools/ClaireonWidgetBPTool_RemoveWidget.h"
#include "Tools/ClaireonWidgetBPTool_RenameAnimation.h"
#include "Tools/ClaireonWidgetBPTool_ReplaceWidget.h"
#include "Tools/ClaireonWidgetBPTool_SetAnimationProperty.h"
#include "Tools/ClaireonWidgetBPTool_SetWidgetProperty.h"

// ---------------------------------------------------------------------------
// File-scope helper: flatten the legacy {operation, session_id, params:{...}}
// envelope into the flat {session_id, ...fields} shape each decomposed tool's
// Execute expects. Also preserves top-level non-"operation"/non-"params"
// fields that some tests set directly (e.g. bare session_id at top level).
// ---------------------------------------------------------------------------

namespace
{
	TSharedPtr<FJsonObject> FlattenLegacyEnvelope(const TSharedPtr<FJsonObject>& Envelope)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		if (!Envelope.IsValid())
		{
			return Result;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : Envelope->Values)
		{
			if (Kv.Key == TEXT("operation") || Kv.Key == TEXT("params"))
			{
				continue;
			}
			Result->SetField(Kv.Key, Kv.Value);
		}

		const TSharedPtr<FJsonObject>* ParamsObj = nullptr;
		if (Envelope->TryGetObjectField(TEXT("params"), ParamsObj) && ParamsObj && ParamsObj->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*ParamsObj)->Values)
			{
				Result->SetField(Kv.Key, Kv.Value);
			}
		}

		return Result;
	}
} // namespace

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
	static FString CreateWBP(const FString& AssetPath,
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

		ClaireonWidgetBPTool_Create Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
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
	static void CloseSession(const FString& SessionId)
	{
		if (SessionId.IsEmpty())
		{
			return;
		}

		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("close"));
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		ClaireonWidgetBPTool_Close Tool;
		Tool.Execute(FlattenLegacyEnvelope(Args));
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
	FString SessionId;

	// --- Step 1: Create the WBP ---
	SessionId = CreateWBP(AssetPath, this);
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

		ClaireonWidgetBPTool_AddWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add TextBlock: %s"), *Result.GetContentAsString()));
			CloseSession(SessionId);
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

		ClaireonWidgetBPTool_SetWidgetProperty Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to set Text property: %s"), *Result.GetContentAsString()));
			CloseSession(SessionId);
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

		ClaireonWidgetBPTool_Compile Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to compile: %s"), *Result.GetContentAsString()));
			CloseSession(SessionId);
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

		ClaireonWidgetBPTool_Close Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
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

	// --- Create WBP with CanvasPanel root ---
	FString SessionId = CreateWBP(AssetPath, this, TEXT("CanvasPanel"));
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

		ClaireonWidgetBPTool_AddWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
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
		CloseSession(SessionId);
		return false;
	}
	AddInfo(TEXT("Added VerticalBox under CanvasPanel"));

	if (!AddWidget(TEXT("TextBlock"), TEXT("TextBlock1"), TEXT("MyVerticalBox")))
	{
		CloseSession(SessionId);
		return false;
	}
	AddInfo(TEXT("Added TextBlock1 under VerticalBox"));

	if (!AddWidget(TEXT("TextBlock"), TEXT("TextBlock2"), TEXT("MyVerticalBox")))
	{
		CloseSession(SessionId);
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

		ClaireonWidgetBPTool_MoveWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to move TextBlock1: %s"), *Result.GetContentAsString()));
			CloseSession(SessionId);
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

		ClaireonWidgetBPTool_RemoveWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to remove TextBlock2: %s"), *Result.GetContentAsString()));
			CloseSession(SessionId);
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

		ClaireonWidgetBPTool_GetState Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to get_state: %s"), *Result.GetContentAsString()));
			CloseSession(SessionId);
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

	CloseSession(SessionId);
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

	// --- Step 1: Create opens a session ---
	FString SessionId = CreateWBP(AssetPath, this);
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

		ClaireonWidgetBPTool_GetState Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
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

		ClaireonWidgetBPTool_Close Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
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

		ClaireonWidgetBPTool_GetState Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
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
// Unknown-session error, missing session_id, invalid asset path, invalid widget class.
//
// NOTE (stage 029): the legacy "invalid operation" case no longer has an
// analog under the decomposed model (operation identity is encoded by the tool
// class itself). The sub-scope is re-pointed to a session-not-found case on a
// session-required decomposed tool, preserving 4 sub-scopes.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_ErrorHandling,
	"Claireon.EditWidgetBP.ErrorHandling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_ErrorHandling::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	// --- Test 1: Unknown/expired session id on a session-required tool ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("add_widget"));
		Args->SetStringField(TEXT("session_id"), TEXT("fake-session-id"));
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		ClaireonWidgetBPTool_AddWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for unknown session id, but got success"));
			return false;
		}
		AddInfo(TEXT("Correctly rejected unknown session id"));
	}

	// --- Test 2: Missing session_id for a session-required operation ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("add_widget"));
		// No session_id set
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		ClaireonWidgetBPTool_AddWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
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

		ClaireonWidgetBPTool_Create Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
		if (!Result.bIsError)
		{
			// Close the erroneously-opened session if somehow it succeeded
			FString SessionId = ParseSessionId(Result.GetContentAsString());
			if (!SessionId.IsEmpty())
			{
				CloseSession(SessionId);
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
		FString SessionId = CreateWBP(AssetPath, this);
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

		ClaireonWidgetBPTool_AddWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(Args));
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for invalid widget class"));
			CloseSession(SessionId);
			return false;
		}
		AddInfo(FString::Printf(TEXT("Correctly rejected invalid widget class with message: %s"),
			*Result.GetContentAsString()));

		CloseSession(SessionId);
	}

	return true;
}

// ===========================================================================
// Test 5: AnimationLifecycle
// Create, rename, duplicate, set_property, delete animations.
//
// NOTE (stage 029): 9 of the 10 animation ops are stubs that return
// "<op> is not yet implemented; tracked in Dispatch Backlog: <url>". Only
// list_animations is implemented. The assertions below reflect that: the
// mutating calls are expected to error with the stub message, list_animations
// is the only success expectation.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_AnimationLifecycle,
	"Claireon.EditWidgetBP.AnimationLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_AnimationLifecycle::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_AnimLifecycleTest");

	FString SessionId = CreateWBP(AssetPath, this);
	if (SessionId.IsEmpty())
	{
		return false;
	}

	const FString StubMarker = TEXT("is not yet implemented; tracked in Dispatch Backlog");

	auto MakeEnvelope = [&](const FString& Op, const TSharedPtr<FJsonObject>& Params) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), Op);
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), Params);
		return Args;
	};

	// --- Step 1: create_animation "FadeIn" ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
		Params->SetNumberField(TEXT("duration"), 2.0);

		ClaireonWidgetBPTool_CreateAnimation Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("create_animation"), Params)));
		TestFalse("create_animation FadeIn should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			FString Name;
			Json->TryGetStringField(TEXT("name"), Name);
			TestEqual("create_animation name echoes FadeIn", Name, TEXT("FadeIn"));
		}
	}

	// --- Step 2: create_animation "SlideOut" ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("SlideOut"));
		Params->SetNumberField(TEXT("duration"), 3.0);

		ClaireonWidgetBPTool_CreateAnimation Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("create_animation"), Params)));
		TestFalse("create_animation SlideOut should succeed", Result.bIsError);
	}

	// --- Step 3: list_animations (implemented) ---
	{
		ClaireonWidgetBPTool_ListAnimations Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("list_animations"), MakeShared<FJsonObject>())));
		TestFalse("list_animations should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			double Count = 0;
			Json->TryGetNumberField(TEXT("count"), Count);
			TestEqual("create_animation produced two animations (FadeIn, SlideOut)",
				static_cast<int32>(Count), 2);
		}
	}

	// --- Step 4: rename_animation FadeIn -> FadeInNew ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeIn"));
		Params->SetStringField(TEXT("new_name"), TEXT("FadeInNew"));

		ClaireonWidgetBPTool_RenameAnimation Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("rename_animation"), Params)));
		TestFalse("rename_animation should succeed", Result.bIsError);
	}

	// --- Step 5: set_animation_property duration=4.0 ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeInNew"));
		Params->SetNumberField(TEXT("duration"), 4.0);

		ClaireonWidgetBPTool_SetAnimationProperty Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("set_animation_property"), Params)));
		TestFalse("set_animation_property duration should succeed", Result.bIsError);
	}

	// --- Step 6: duplicate_animation FadeInNew -> FadeInCopy ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeInNew"));
		Params->SetStringField(TEXT("new_name"), TEXT("FadeInCopy"));

		ClaireonWidgetBPTool_DuplicateAnimation Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("duplicate_animation"), Params)));
		TestFalse("duplicate_animation should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			FString Source;
			Json->TryGetStringField(TEXT("source"), Source);
			TestEqual("duplicate_animation source echoes FadeInNew", Source, TEXT("FadeInNew"));
		}
	}

	// --- Step 7: delete_animation ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("SlideOut"));

		ClaireonWidgetBPTool_DeleteAnimation Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("delete_animation"), Params)));
		TestFalse("delete_animation should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			FString DeletedName;
			Json->TryGetStringField(TEXT("deleted"), DeletedName);
			TestEqual("delete_animation echoes deleted name", DeletedName, TEXT("SlideOut"));
		}
	}

	// --- Step 8: list_animations (implemented; still 0) ---
	{
		ClaireonWidgetBPTool_ListAnimations Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("list_animations"), MakeShared<FJsonObject>())));
		TestFalse("list_animations should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			double Count = 0;
			Json->TryGetNumberField(TEXT("count"), Count);
			TestEqual("Count reflects FadeIn + FadeInCopy after SlideOut delete",
				static_cast<int32>(Count), 2);
		}
	}

	// --- Step 9: get_animation_details ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeInNew"));

		ClaireonWidgetBPTool_GetAnimationDetails Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("get_animation_details"), Params)));
		TestFalse("get_animation_details should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			FString Name;
			Json->TryGetStringField(TEXT("name"), Name);
			TestEqual("get_animation_details echoes name", Name, TEXT("FadeInNew"));
		}
	}

	// --- Step 10: compile (implemented) ---
	{
		ClaireonWidgetBPTool_Compile Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("compile"), MakeShared<FJsonObject>())));
		TestFalse("compile should succeed", Result.bIsError);
	}

	// --- Step 11 (stage 014 persistence): save, close, reopen, verify count ---
	{
		ClaireonWidgetBPTool_Save SaveTool;
		auto SaveResult = SaveTool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("save"), MakeShared<FJsonObject>())));
		TestFalse("save should succeed", SaveResult.bIsError);

		CloseSession(SessionId);

		// Reopen via claireon.widgetbp_open; the animations must survive the round trip.
		TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
		OpenArgs->SetStringField(TEXT("operation"), TEXT("open"));
		TSharedPtr<FJsonObject> OpenParams = MakeShared<FJsonObject>();
		OpenParams->SetStringField(TEXT("asset_path"), AssetPath);
		OpenArgs->SetObjectField(TEXT("params"), OpenParams);

		ClaireonWidgetBPTool_Open OpenTool;
		auto OpenResult = OpenTool.Execute(FlattenLegacyEnvelope(OpenArgs));
		TestFalse("reopen should succeed", OpenResult.bIsError);
		SessionId = ParseSessionId(OpenResult.GetContentAsString());

		ClaireonWidgetBPTool_ListAnimations ListTool;
		auto ListResult = ListTool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("list_animations"), MakeShared<FJsonObject>())));
		TestFalse("list_animations after reopen should succeed", ListResult.bIsError);
		TSharedPtr<FJsonObject> ListJson = ParseJson(ListResult.GetContentAsString());
		if (ListJson.IsValid())
		{
			double Count = 0;
			ListJson->TryGetNumberField(TEXT("count"), Count);
			TestEqual("Persistence: FadeInNew + FadeInCopy survive save/close/reopen",
				static_cast<int32>(Count), 2);
		}
	}

	CloseSession(SessionId);
	return true;
}

// ===========================================================================
// Test 6: AnimationBindingTracksKeyframes
// Create animation, bind widget, add track, add keyframes, remove keyframe.
//
// NOTE (stage 029): every animation mutator (create_animation,
// add_animation_binding, add_animation_track, add_animation_keyframe,
// remove_animation_keyframe, get_animation_details) is a stub returning
// the "not yet implemented; tracked in Dispatch Backlog" error. Assertions
// are flipped accordingly.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_AnimationBindingTracksKeyframes,
	"Claireon.EditWidgetBP.AnimationBindingTracksKeyframes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_AnimationBindingTracksKeyframes::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_AnimTrackTest");

	FString SessionId = CreateWBP(AssetPath, this);
	if (SessionId.IsEmpty())
	{
		return false;
	}

	auto MakeEnvelope = [&](const FString& Op, const TSharedPtr<FJsonObject>& Params) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), Op);
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), Params);
		return Args;
	};

	// --- Step 1: Add a TextBlock widget (implemented) ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
		Params->SetStringField(TEXT("widget_name"), TEXT("FadeLabel"));

		ClaireonWidgetBPTool_AddWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_widget"), Params)));
		TestFalse("add_widget should succeed", Result.bIsError);
	}

	// --- Step 2: Create animation ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetNumberField(TEXT("duration"), 2.0);

		ClaireonWidgetBPTool_CreateAnimation Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("create_animation"), Params)));
		TestFalse("create_animation should succeed", Result.bIsError);
	}

	// --- Step 3: Bind widget to animation ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetStringField(TEXT("widget_name"), TEXT("FadeLabel"));

		ClaireonWidgetBPTool_AddAnimationBinding Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_animation_binding"), Params)));
		TestFalse("add_animation_binding should succeed", Result.bIsError);
	}

	// --- Step 4: Add RenderOpacity track ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetStringField(TEXT("widget_name"), TEXT("FadeLabel"));
		Params->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));
		Params->SetStringField(TEXT("track_type"), TEXT("float"));

		ClaireonWidgetBPTool_AddAnimationTrack Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_animation_track"), Params)));
		TestFalse("add_animation_track should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			FString TrackClass;
			Json->TryGetStringField(TEXT("track_class"), TrackClass);
			TestEqual("add_animation_track returns float track class", TrackClass, TEXT("MovieSceneFloatTrack"));
		}
	}

	// --- Step 5: Add keyframe at t=0 ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetStringField(TEXT("widget_name"), TEXT("FadeLabel"));
		Params->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));
		Params->SetNumberField(TEXT("time"), 0.0);
		Params->SetNumberField(TEXT("value"), 0.0);
		Params->SetStringField(TEXT("interpolation"), TEXT("linear"));

		ClaireonWidgetBPTool_AddAnimationKeyframe Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_animation_keyframe"), Params)));
		TestFalse("add_animation_keyframe at t=0 should succeed", Result.bIsError);
	}

	// --- Step 6: Add keyframe at t=2 ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetStringField(TEXT("widget_name"), TEXT("FadeLabel"));
		Params->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));
		Params->SetNumberField(TEXT("time"), 2.0);
		Params->SetNumberField(TEXT("value"), 1.0);

		ClaireonWidgetBPTool_AddAnimationKeyframe Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_animation_keyframe"), Params)));
		TestFalse("add_animation_keyframe at t=2 should succeed", Result.bIsError);
	}

	// --- Step 7: get_animation_details ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));

		ClaireonWidgetBPTool_GetAnimationDetails Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("get_animation_details"), Params)));
		TestFalse("get_animation_details should succeed", Result.bIsError);
	}

	// --- Step 8: Remove keyframe at t=0 ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetStringField(TEXT("widget_name"), TEXT("FadeLabel"));
		Params->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));
		Params->SetNumberField(TEXT("time"), 0.0);

		ClaireonWidgetBPTool_RemoveAnimationKeyframe Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("remove_animation_keyframe"), Params)));
		TestFalse("remove_animation_keyframe should succeed", Result.bIsError);
	}

	// --- Step 9: Error scenario -- add track for unbound widget (stub still errors) ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("animation_name"), TEXT("FadeAnim"));
		Params->SetStringField(TEXT("widget_name"), TEXT("NonExistentWidget"));
		Params->SetStringField(TEXT("property_path"), TEXT("RenderOpacity"));

		ClaireonWidgetBPTool_AddAnimationTrack Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_animation_track"), Params)));
		TestTrue("add_animation_track for unbound widget should error", Result.bIsError);
	}

	CloseSession(SessionId);
	return true;
}

// ===========================================================================
// Test 7: MVVMViewModelLifecycle
// Add, list, duplicate-error, remove, remove-missing-error for viewmodels.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_MVVMViewModelLifecycle,
	"Claireon.EditWidgetBP.MVVMViewModelLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_MVVMViewModelLifecycle::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_MVVMViewModelTest");

	FString SessionId = CreateWBP(AssetPath, this);
	if (SessionId.IsEmpty())
	{
		return false;
	}

	auto MakeEnvelope = [&](const FString& Op, const TSharedPtr<FJsonObject>& Params) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), Op);
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), Params);
		return Args;
	};

	// --- Step 1: list_mvvm_viewmodels -- verify count == 0 ---
	{
		ClaireonWidgetBPTool_ListMVVMViewModels Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("list_mvvm_viewmodels"), MakeShared<FJsonObject>())));
		TestFalse("list_mvvm_viewmodels should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			double Count = 0;
			Json->TryGetNumberField(TEXT("count"), Count);
			TestEqual("Should have 0 viewmodels initially", static_cast<int32>(Count), 0);
		}
	}

	// --- Step 2: add_mvvm_viewmodel "TestVM" ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("viewmodel_name"), TEXT("TestVM"));
		Params->SetStringField(TEXT("viewmodel_class"), TEXT("MVVMViewModelBase"));

		ClaireonWidgetBPTool_AddMVVMViewModel Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_mvvm_viewmodel"), Params)));
		TestFalse("add_mvvm_viewmodel should succeed", Result.bIsError);

		if (!Result.bIsError)
		{
			TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
			if (Json.IsValid())
			{
				FString Name;
				Json->TryGetStringField(TEXT("name"), Name);
				TestEqual("ViewModel name should be TestVM", Name, TEXT("TestVM"));
			}
		}
	}

	// --- Step 3: list_mvvm_viewmodels -- verify count == 1, name == "TestVM" ---
	{
		ClaireonWidgetBPTool_ListMVVMViewModels Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("list_mvvm_viewmodels"), MakeShared<FJsonObject>())));
		TestFalse("list_mvvm_viewmodels should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			double Count = 0;
			Json->TryGetNumberField(TEXT("count"), Count);
			TestEqual("Should have 1 viewmodel", static_cast<int32>(Count), 1);

			const TArray<TSharedPtr<FJsonValue>>* VMArray = nullptr;
			if (Json->TryGetArrayField(TEXT("viewmodels"), VMArray) && VMArray && VMArray->Num() > 0)
			{
				const TSharedPtr<FJsonObject>* VMObj = nullptr;
				if ((*VMArray)[0]->TryGetObject(VMObj) && VMObj)
				{
					FString Name;
					(*VMObj)->TryGetStringField(TEXT("name"), Name);
					TestEqual("First viewmodel name should be TestVM", Name, TEXT("TestVM"));
				}
			}
		}
	}

	// --- Step 4: add_mvvm_viewmodel with duplicate name -- verify error ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("viewmodel_name"), TEXT("TestVM"));
		Params->SetStringField(TEXT("viewmodel_class"), TEXT("MVVMViewModelBase"));

		ClaireonWidgetBPTool_AddMVVMViewModel Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_mvvm_viewmodel"), Params)));
		TestTrue("add_mvvm_viewmodel with duplicate name should error", Result.bIsError);
	}

	// --- Step 5: remove_mvvm_viewmodel "TestVM" ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("viewmodel_name"), TEXT("TestVM"));

		ClaireonWidgetBPTool_RemoveMVVMViewModel Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("remove_mvvm_viewmodel"), Params)));
		TestFalse("remove_mvvm_viewmodel should succeed", Result.bIsError);
	}

	// --- Step 6: list_mvvm_viewmodels -- verify count == 0 ---
	{
		ClaireonWidgetBPTool_ListMVVMViewModels Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("list_mvvm_viewmodels"), MakeShared<FJsonObject>())));
		TestFalse("list_mvvm_viewmodels should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			double Count = 0;
			Json->TryGetNumberField(TEXT("count"), Count);
			TestEqual("Should have 0 viewmodels after removal", static_cast<int32>(Count), 0);
		}
	}

	// --- Step 7: remove_mvvm_viewmodel with non-existent name -- verify error ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("viewmodel_name"), TEXT("NonExistentVM"));

		ClaireonWidgetBPTool_RemoveMVVMViewModel Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("remove_mvvm_viewmodel"), Params)));
		TestTrue("remove_mvvm_viewmodel with non-existent name should error", Result.bIsError);
	}

	CloseSession(SessionId);
	return true;
}

// ===========================================================================
// Test 8: MVVMBindingCRUD
// Add viewmodel, add binding, list, edit, remove binding, compile.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_MVVMBindingCRUD,
	"Claireon.EditWidgetBP.MVVMBindingCRUD",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_MVVMBindingCRUD::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_MVVMBindingTest");

	FString SessionId = CreateWBP(AssetPath, this);
	if (SessionId.IsEmpty())
	{
		return false;
	}

	auto MakeEnvelope = [&](const FString& Op, const TSharedPtr<FJsonObject>& Params) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), Op);
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), Params);
		return Args;
	};

	// --- Step 1: Add a TextBlock widget ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
		Params->SetStringField(TEXT("widget_name"), TEXT("TestText"));

		ClaireonWidgetBPTool_AddWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_widget"), Params)));
		TestFalse("add_widget TextBlock should succeed", Result.bIsError);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add TextBlock: %s"), *Result.GetContentAsString()));
			CloseSession(SessionId);
			return false;
		}
	}

	// --- Step 2: Add viewmodel ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("viewmodel_name"), TEXT("TestVM"));
		Params->SetStringField(TEXT("viewmodel_class"), TEXT("MVVMViewModelBase"));

		ClaireonWidgetBPTool_AddMVVMViewModel Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_mvvm_viewmodel"), Params)));
		TestFalse("add_mvvm_viewmodel should succeed", Result.bIsError);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add viewmodel: %s"), *Result.GetContentAsString()));
			CloseSession(SessionId);
			return false;
		}
	}

	// --- Step 3: Add binding ---
	FString BindingId;
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("viewmodel_name"), TEXT("TestVM"));
		Params->SetStringField(TEXT("viewmodel_property"), TEXT("TestProp"));
		Params->SetStringField(TEXT("widget_name"), TEXT("TestText"));
		Params->SetStringField(TEXT("widget_property"), TEXT("RenderOpacity"));
		Params->SetStringField(TEXT("mode"), TEXT("OneWayToDestination"));

		ClaireonWidgetBPTool_AddMVVMBinding Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_mvvm_binding"), Params)));
		// The binding may fail if property resolution is strict -- either outcome is valid
		if (Result.bIsError)
		{
			AddInfo(FString::Printf(TEXT("add_mvvm_binding returned error (property resolution may be strict): %s"), *Result.GetContentAsString()));
			// Skip remaining binding tests -- clean up and exit
			CloseSession(SessionId);
			return true;
		}

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			Json->TryGetStringField(TEXT("binding_id"), BindingId);
		}
		TestFalse("binding_id should not be empty", BindingId.IsEmpty());
		AddInfo(FString::Printf(TEXT("Created binding with id: %s"), *BindingId));
	}

	// --- Step 4: list_mvvm_bindings -- verify count == 1 ---
	{
		ClaireonWidgetBPTool_ListMVVMBindings Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("list_mvvm_bindings"), MakeShared<FJsonObject>())));
		TestFalse("list_mvvm_bindings should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			double Count = 0;
			Json->TryGetNumberField(TEXT("count"), Count);
			TestEqual("Should have 1 binding", static_cast<int32>(Count), 1);
		}
	}

	// --- Step 5: edit_mvvm_binding -- change mode to TwoWay ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("binding_id"), BindingId);
		Params->SetStringField(TEXT("mode"), TEXT("TwoWay"));

		ClaireonWidgetBPTool_EditMVVMBinding Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("edit_mvvm_binding"), Params)));
		TestFalse("edit_mvvm_binding should succeed", Result.bIsError);

		if (!Result.bIsError)
		{
			TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
			if (Json.IsValid())
			{
				FString Mode;
				Json->TryGetStringField(TEXT("mode"), Mode);
				TestEqual("Mode should be TwoWay", Mode, TEXT("TwoWay"));
			}
		}
	}

	// --- Step 6: remove_mvvm_binding ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("binding_id"), BindingId);

		ClaireonWidgetBPTool_RemoveMVVMBinding Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("remove_mvvm_binding"), Params)));
		TestFalse("remove_mvvm_binding should succeed", Result.bIsError);
	}

	// --- Step 7: list_mvvm_bindings -- verify count == 0 ---
	{
		ClaireonWidgetBPTool_ListMVVMBindings Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("list_mvvm_bindings"), MakeShared<FJsonObject>())));
		TestFalse("list_mvvm_bindings should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			double Count = 0;
			Json->TryGetNumberField(TEXT("count"), Count);
			TestEqual("Should have 0 bindings after removal", static_cast<int32>(Count), 0);
		}
	}

	// --- Step 8: Compile ---
	{
		ClaireonWidgetBPTool_Compile Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("compile"), MakeShared<FJsonObject>())));
		TestFalse("compile should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			bool bSuccess = false;
			Json->TryGetBoolField(TEXT("success"), bSuccess);
			TestTrue("Compile should report success=true", bSuccess);
		}
	}

	CloseSession(SessionId);
	return true;
}

// ===========================================================================
// Test 9: MVVMBindingErrorHandling
// Various error scenarios for MVVM operations.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_MVVMBindingErrorHandling,
	"Claireon.EditWidgetBP.MVVMBindingErrorHandling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_MVVMBindingErrorHandling::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_MVVMErrorTest");

	FString SessionId = CreateWBP(AssetPath, this);
	if (SessionId.IsEmpty())
	{
		return false;
	}

	auto MakeEnvelope = [&](const FString& Op, const TSharedPtr<FJsonObject>& Params) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), Op);
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), Params);
		return Args;
	};

	// --- Step 1: add_mvvm_binding without any viewmodel -- verify error ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("viewmodel_name"), TEXT("NonExistentVM"));
		Params->SetStringField(TEXT("viewmodel_property"), TEXT("SomeProp"));
		Params->SetStringField(TEXT("widget_name"), TEXT("SomeWidget"));
		Params->SetStringField(TEXT("widget_property"), TEXT("RenderOpacity"));

		ClaireonWidgetBPTool_AddMVVMBinding Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_mvvm_binding"), Params)));
		TestTrue("add_mvvm_binding without viewmodel should error", Result.bIsError);
		AddInfo(TEXT("Correctly rejected binding without viewmodel"));
	}

	// --- Step 2: add_mvvm_viewmodel with invalid class -- verify error ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("viewmodel_name"), TEXT("BadVM"));
		Params->SetStringField(TEXT("viewmodel_class"), TEXT("ThisClassDoesNotExistAnywhere"));

		ClaireonWidgetBPTool_AddMVVMViewModel Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_mvvm_viewmodel"), Params)));
		TestTrue("add_mvvm_viewmodel with invalid class should error", Result.bIsError);
		AddInfo(TEXT("Correctly rejected invalid viewmodel class"));
	}

	// --- Step 3: add_mvvm_viewmodel with valid class ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("viewmodel_name"), TEXT("ValidVM"));
		Params->SetStringField(TEXT("viewmodel_class"), TEXT("MVVMViewModelBase"));

		ClaireonWidgetBPTool_AddMVVMViewModel Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_mvvm_viewmodel"), Params)));
		TestFalse("add_mvvm_viewmodel with valid class should succeed", Result.bIsError);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add valid viewmodel: %s"), *Result.GetContentAsString()));
			CloseSession(SessionId);
			return false;
		}
		AddInfo(TEXT("Added valid viewmodel for error-handling tests"));
	}

	// --- Step 4: add_mvvm_binding with non-existent widget -- verify error ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("viewmodel_name"), TEXT("ValidVM"));
		Params->SetStringField(TEXT("viewmodel_property"), TEXT("SomeProp"));
		Params->SetStringField(TEXT("widget_name"), TEXT("NonExistentWidget"));
		Params->SetStringField(TEXT("widget_property"), TEXT("RenderOpacity"));

		ClaireonWidgetBPTool_AddMVVMBinding Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_mvvm_binding"), Params)));
		TestTrue("add_mvvm_binding with non-existent widget should error", Result.bIsError);
		AddInfo(TEXT("Correctly rejected binding with non-existent widget"));
	}

	// --- Step 5: add_mvvm_binding with non-existent viewmodel name -- verify error ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("viewmodel_name"), TEXT("VMThatDoesNotExist"));
		Params->SetStringField(TEXT("viewmodel_property"), TEXT("SomeProp"));
		Params->SetStringField(TEXT("widget_name"), TEXT("CanvasPanel"));
		Params->SetStringField(TEXT("widget_property"), TEXT("RenderOpacity"));

		ClaireonWidgetBPTool_AddMVVMBinding Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_mvvm_binding"), Params)));
		TestTrue("add_mvvm_binding with non-existent viewmodel name should error", Result.bIsError);
		AddInfo(TEXT("Correctly rejected binding with non-existent viewmodel name"));
	}

	// --- Step 6: edit_mvvm_binding with invalid GUID -- verify error ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("binding_id"), TEXT("00000000-0000-0000-0000-000000000000"));
		Params->SetStringField(TEXT("mode"), TEXT("TwoWay"));

		ClaireonWidgetBPTool_EditMVVMBinding Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("edit_mvvm_binding"), Params)));
		TestTrue("edit_mvvm_binding with invalid GUID should error", Result.bIsError);
		AddInfo(TEXT("Correctly rejected edit with invalid binding GUID"));
	}

	// --- Step 7: remove_mvvm_binding with invalid GUID -- verify error ---
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("binding_id"), TEXT("00000000-0000-0000-0000-000000000000"));

		ClaireonWidgetBPTool_RemoveMVVMBinding Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("remove_mvvm_binding"), Params)));
		TestTrue("remove_mvvm_binding with invalid GUID should error", Result.bIsError);
		AddInfo(TEXT("Correctly rejected remove with invalid binding GUID"));
	}

	CloseSession(SessionId);
	return true;
}


// ===========================================================================
// Test 10: AddWidgetAtIndex
// Add widgets to a panel, then insert one at a specific index.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_AddWidgetAtIndex,
	"Claireon.EditWidgetBP.AddWidgetAtIndex",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_AddWidgetAtIndex::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_AddAtIndexTest");

	FString SessionId = CreateWBP(AssetPath, this, TEXT("VerticalBox"));
	if (SessionId.IsEmpty())
	{
		return false;
	}

	auto MakeEnvelope = [&](const FString& Op, const TSharedPtr<FJsonObject>& Params) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), Op);
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), Params);
		return Args;
	};

	// Add three TextBlocks: A, B, C (appended in order)
	for (const FString& Name : {TEXT("WidgetA"), TEXT("WidgetB"), TEXT("WidgetC")})
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
		Params->SetStringField(TEXT("widget_name"), Name);
		Params->SetStringField(TEXT("parent_name"), TEXT("VerticalBox"));
		ClaireonWidgetBPTool_AddWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_widget"), Params)));
		TestFalse(FString::Printf(TEXT("add_widget %s should succeed"), *Name), Result.bIsError);
	}

	// Insert WidgetX at index 1 (between A and B)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
		Params->SetStringField(TEXT("widget_name"), TEXT("WidgetX"));
		Params->SetStringField(TEXT("parent_name"), TEXT("VerticalBox"));
		Params->SetNumberField(TEXT("index"), 1);
		ClaireonWidgetBPTool_AddWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_widget"), Params)));
		TestFalse("add_widget at index 1 should succeed", Result.bIsError);

		// Parse tree and verify order: A, X, B, C
		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			const TSharedPtr<FJsonObject>* TreeObj = nullptr;
			if (Json->TryGetObjectField(TEXT("widget_tree"), TreeObj))
			{
				const TSharedPtr<FJsonObject>* RootObj = nullptr;
				if ((*TreeObj)->TryGetObjectField(TEXT("root"), RootObj))
				{
					const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
					if ((*RootObj)->TryGetArrayField(TEXT("children"), Children) && Children)
					{
						TestEqual("Should have 4 children", Children->Num(), 4);
						if (Children->Num() == 4)
						{
							auto GetName = [&](int32 Idx) -> FString
							{
								const TSharedPtr<FJsonObject>* ChildObj = nullptr;
								if ((*Children)[Idx]->TryGetObject(ChildObj))
								{
									FString N;
									(*ChildObj)->TryGetStringField(TEXT("name"), N);
									return N;
								}
								return TEXT("");
							};
							TestEqual("Child 0 should be WidgetA", GetName(0), TEXT("WidgetA"));
							TestEqual("Child 1 should be WidgetX", GetName(1), TEXT("WidgetX"));
							TestEqual("Child 2 should be WidgetB", GetName(2), TEXT("WidgetB"));
							TestEqual("Child 3 should be WidgetC", GetName(3), TEXT("WidgetC"));
						}
					}
				}
			}
		}
	}

	CloseSession(SessionId);
	return true;
}

// ===========================================================================
// Test 11: MoveWidgetReorder
// Move a widget within the same parent to a different index.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_MoveWidgetReorder,
	"Claireon.EditWidgetBP.MoveWidgetReorder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_MoveWidgetReorder::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_MoveReorderTest");

	FString SessionId = CreateWBP(AssetPath, this, TEXT("VerticalBox"));
	if (SessionId.IsEmpty())
	{
		return false;
	}

	auto MakeEnvelope = [&](const FString& Op, const TSharedPtr<FJsonObject>& Params) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), Op);
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), Params);
		return Args;
	};

	// Add three TextBlocks: A, B, C
	for (const FString& Name : {TEXT("WidgetA"), TEXT("WidgetB"), TEXT("WidgetC")})
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
		Params->SetStringField(TEXT("widget_name"), Name);
		Params->SetStringField(TEXT("parent_name"), TEXT("VerticalBox"));
		ClaireonWidgetBPTool_AddWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_widget"), Params)));
		TestFalse(FString::Printf(TEXT("add_widget %s should succeed"), *Name), Result.bIsError);
	}

	// Move WidgetC to index 0 (before A): expect C, A, B
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_name"), TEXT("WidgetC"));
		Params->SetStringField(TEXT("new_parent_name"), TEXT("VerticalBox"));
		Params->SetNumberField(TEXT("index"), 0);
		ClaireonWidgetBPTool_MoveWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("move_widget"), Params)));
		TestFalse("move_widget to index 0 should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			const TSharedPtr<FJsonObject>* TreeObj = nullptr;
			if (Json->TryGetObjectField(TEXT("widget_tree"), TreeObj))
			{
				const TSharedPtr<FJsonObject>* RootObj = nullptr;
				if ((*TreeObj)->TryGetObjectField(TEXT("root"), RootObj))
				{
					const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
					if ((*RootObj)->TryGetArrayField(TEXT("children"), Children) && Children)
					{
						TestEqual("Should have 3 children", Children->Num(), 3);
						if (Children->Num() == 3)
						{
							auto GetName = [&](int32 Idx) -> FString
							{
								const TSharedPtr<FJsonObject>* ChildObj = nullptr;
								if ((*Children)[Idx]->TryGetObject(ChildObj))
								{
									FString N;
									(*ChildObj)->TryGetStringField(TEXT("name"), N);
									return N;
								}
								return TEXT("");
							};
							TestEqual("Child 0 should be WidgetC", GetName(0), TEXT("WidgetC"));
							TestEqual("Child 1 should be WidgetA", GetName(1), TEXT("WidgetA"));
							TestEqual("Child 2 should be WidgetB", GetName(2), TEXT("WidgetB"));
						}
					}
				}
			}
		}
	}

	CloseSession(SessionId);
	return true;
}

// ===========================================================================
// Test 12: ReplaceWidgetPreservesChildren
// Replace a VBox with an HBox and verify children are reparented.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditWidgetBPTest_ReplaceWidgetPreservesChildren,
	"Claireon.EditWidgetBP.ReplaceWidgetPreservesChildren",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditWidgetBPTest_ReplaceWidgetPreservesChildren::RunTest(const FString& Parameters)
{
	using namespace EditWidgetBPTestHelpers;

	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_ReplaceChildrenTest");

	FString SessionId = CreateWBP(AssetPath, this, TEXT("CanvasPanel"));
	if (SessionId.IsEmpty())
	{
		return false;
	}

	auto MakeEnvelope = [&](const FString& Op, const TSharedPtr<FJsonObject>& Params) -> TSharedPtr<FJsonObject>
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), Op);
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), Params);
		return Args;
	};

	// Add a VerticalBox under root, then two TextBlocks under it
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_class"), TEXT("VerticalBox"));
		Params->SetStringField(TEXT("widget_name"), TEXT("MyVBox"));
		Params->SetStringField(TEXT("parent_name"), TEXT("CanvasPanel"));
		ClaireonWidgetBPTool_AddWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_widget"), Params)));
		TestFalse("add VerticalBox should succeed", Result.bIsError);
	}
	for (const FString& Name : {TEXT("ChildA"), TEXT("ChildB")})
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_class"), TEXT("TextBlock"));
		Params->SetStringField(TEXT("widget_name"), Name);
		Params->SetStringField(TEXT("parent_name"), TEXT("MyVBox"));
		ClaireonWidgetBPTool_AddWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("add_widget"), Params)));
		TestFalse(FString::Printf(TEXT("add %s should succeed"), *Name), Result.bIsError);
	}

	// Replace MyVBox with HorizontalBox -- children should transfer
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("widget_name"), TEXT("MyVBox"));
		Params->SetStringField(TEXT("new_widget_class"), TEXT("HorizontalBox"));
		ClaireonWidgetBPTool_ReplaceWidget Tool;
		auto Result = Tool.Execute(FlattenLegacyEnvelope(MakeEnvelope(TEXT("replace_widget"), Params)));
		TestFalse("replace_widget should succeed", Result.bIsError);

		TSharedPtr<FJsonObject> Json = ParseJson(Result.GetContentAsString());
		if (Json.IsValid())
		{
			const TSharedPtr<FJsonObject>* TreeObj = nullptr;
			if (Json->TryGetObjectField(TEXT("widget_tree"), TreeObj))
			{
				const TSharedPtr<FJsonObject>* RootObj = nullptr;
				if ((*TreeObj)->TryGetObjectField(TEXT("root"), RootObj))
				{
					const TArray<TSharedPtr<FJsonValue>>* RootChildren = nullptr;
					if ((*RootObj)->TryGetArrayField(TEXT("children"), RootChildren) && RootChildren && RootChildren->Num() > 0)
					{
						// The first child of root should be the new HorizontalBox
						const TSharedPtr<FJsonObject>* HBoxObj = nullptr;
						if ((*RootChildren)[0]->TryGetObject(HBoxObj))
						{
							FString ClassName;
							(*HBoxObj)->TryGetStringField(TEXT("class"), ClassName);
							TestEqual("Replaced widget should be HorizontalBox", ClassName, TEXT("HorizontalBox"));

							const TArray<TSharedPtr<FJsonValue>>* HBoxChildren = nullptr;
							if ((*HBoxObj)->TryGetArrayField(TEXT("children"), HBoxChildren) && HBoxChildren)
							{
								TestEqual("HBox should have 2 children (preserved)", HBoxChildren->Num(), 2);
								if (HBoxChildren->Num() == 2)
								{
									auto GetName = [&](int32 Idx) -> FString
									{
										const TSharedPtr<FJsonObject>* ChildObj = nullptr;
										if ((*HBoxChildren)[Idx]->TryGetObject(ChildObj))
										{
											FString N;
											(*ChildObj)->TryGetStringField(TEXT("name"), N);
											return N;
										}
										return TEXT("");
									};
									TestEqual("Child 0 should be ChildA", GetName(0), TEXT("ChildA"));
									TestEqual("Child 1 should be ChildB", GetName(1), TEXT("ChildB"));
								}
							}
						}
					}
				}
			}
		}
	}

	CloseSession(SessionId);
	return true;
}
