// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonTool_InputInspect.h"
#include "Tools/ClaireonInputTool_Create.h"
#include "Tools/ClaireonInputTool_Close.h"
#include "Tools/ClaireonInputTool_Status.h"
#include "Tools/ClaireonInputTool_Save.h"
#include "Tools/ClaireonInputTool_SetValueType.h"
#include "Tools/ClaireonInputTool_AddActionTrigger.h"
#include "Tools/ClaireonInputTool_RemoveActionTrigger.h"
#include "Tools/ClaireonInputTool_AddActionModifier.h"
#include "Tools/ClaireonInputTool_RemoveActionModifier.h"
#include "Tools/ClaireonInputTool_AddMapping.h"
#include "Tools/ClaireonInputTool_RemoveMapping.h"
#include "Tools/ClaireonInputTool_SetMappingKey.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "UObject/Package.h"
#include "AssetRegistry/AssetRegistryModule.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{
	/** Extract session ID from the text-based edit tool response.
	 *  The response contains "Session: <id>" on a line. */
	FString ExtractInputEditSessionId(const IClaireonTool::FToolResult& Result)
	{
		const FString& Output = Result.ErrorMessage;
		static const FString Marker = TEXT("Session: ");
		int32 Start = Output.Find(Marker);
		if (Start == INDEX_NONE)
		{
			return FString();
		}
		Start += Marker.Len();
		int32 End = Output.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Start);
		if (End == INDEX_NONE)
		{
			End = Output.Len();
		}
		return Output.Mid(Start, End - Start).TrimStartAndEnd();
	}

	/** Create a transient test Input Action via the create tool. Returns session ID. */
	FString CreateTestIA(const FString& Path)
	{
		ClaireonInputTool_Create Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), Path);
		Args->SetStringField(TEXT("asset_type"), TEXT("input_action"));
		auto Result = Tool.Execute(Args);
		return ExtractInputEditSessionId(Result);
	}

	/** Create a transient test IMC via the create tool. Returns session ID. */
	FString CreateTestIMC(const FString& Path)
	{
		ClaireonInputTool_Create Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), Path);
		Args->SetStringField(TEXT("asset_type"), TEXT("mapping_context"));
		auto Result = Tool.Execute(Args);
		return ExtractInputEditSessionId(Result);
	}

	/** Close a session. */
	void CloseInputSession(const FString& SessionId)
	{
		ClaireonInputTool_Close Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Tool.Execute(Args);
	}

	/** Execute a tool with args containing session_id and arbitrary extra fields. */
	template <typename TTool>
	IClaireonTool::FToolResult ExecuteInputTool(const FString& SessionId, TSharedPtr<FJsonObject> ExtraArgs = nullptr)
	{
		TTool Tool;
		TSharedPtr<FJsonObject> Args = ExtraArgs.IsValid() ? ExtraArgs : MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		return Tool.Execute(Args);
	}
}

// ============================================================================
// input_inspect
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, InspectMissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_InputInspect Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("asset_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, InspectBadAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_InputInspect Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotExist/IA_Fake"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Failed to load")));
	co_return;
}

// ============================================================================
// input_* -- Error Handling
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, StatusMissingSessionId, UNTEST_TIMEOUTMS(5000))
{
	ClaireonInputTool_Status Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("session_id")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, StatusInvalidSessionId, UNTEST_TIMEOUTMS(5000))
{
	ClaireonInputTool_Status Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), TEXT("nonexistent-session-id"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("not found")));
	co_return;
}

// ============================================================================
// input_* -- Create + Session Lifecycle
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, CreateInputAction, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId = CreateTestIA(TEXT("/Game/__MCPTests/IA_TestCreate"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Status should work
	auto StatusResult = ExecuteInputTool<ClaireonInputTool_Status>(SessionId);
	UNTEST_EXPECT_TRUE(StatusResult.GetContentAsString().Contains(TEXT("Input Action")));

	// Close
	CloseInputSession(SessionId);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, CreateMappingContext, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId = CreateTestIMC(TEXT("/Game/__MCPTests/IMC_TestCreate"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	auto StatusResult = ExecuteInputTool<ClaireonInputTool_Status>(SessionId);
	UNTEST_EXPECT_TRUE(StatusResult.GetContentAsString().Contains(TEXT("Input Mapping Context")));

	CloseInputSession(SessionId);
	co_return;
}

// ============================================================================
// input_set_value_type
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, SetValueType, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId = CreateTestIA(TEXT("/Game/__MCPTests/IA_TestValueType"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("value_type"), TEXT("2d"));
	auto Result = ExecuteInputTool<ClaireonInputTool_SetValueType>(SessionId, Args);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Axis2D")));

	Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("value_type"), TEXT("bool"));
	Result = ExecuteInputTool<ClaireonInputTool_SetValueType>(SessionId, Args);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Boolean")));

	CloseInputSession(SessionId);
	co_return;
}

// ============================================================================
// input_add_action_trigger / input_remove_action_trigger
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, AddRemoveActionTriggers, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId = CreateTestIA(TEXT("/Game/__MCPTests/IA_TestTriggers"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Add Down trigger
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("trigger_class"), TEXT("Down"));
	auto Result = ExecuteInputTool<ClaireonInputTool_AddActionTrigger>(SessionId, Args);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("InputTriggerDown")));

	// Add Hold trigger
	Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("trigger_class"), TEXT("Hold"));
	Result = ExecuteInputTool<ClaireonInputTool_AddActionTrigger>(SessionId, Args);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("InputTriggerHold")));

	// Remove trigger at index 0 (Down)
	Args = MakeShared<FJsonObject>();
	Args->SetNumberField(TEXT("index"), 0);
	Result = ExecuteInputTool<ClaireonInputTool_RemoveActionTrigger>(SessionId, Args);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("InputTriggerHold")));

	// Out of bounds index should error
	Args = MakeShared<FJsonObject>();
	Args->SetNumberField(TEXT("index"), 99);
	Result = ExecuteInputTool<ClaireonInputTool_RemoveActionTrigger>(SessionId, Args);
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("out of range")));

	CloseInputSession(SessionId);
	co_return;
}

// ============================================================================
// input_add_action_modifier / input_remove_action_modifier
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, AddRemoveActionModifiers, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId = CreateTestIA(TEXT("/Game/__MCPTests/IA_TestModifiers"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Add DeadZone modifier
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("modifier_class"), TEXT("DeadZone"));
	auto Result = ExecuteInputTool<ClaireonInputTool_AddActionModifier>(SessionId, Args);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("InputModifierDeadZone")));

	// Add Negate modifier
	Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("modifier_class"), TEXT("Negate"));
	Result = ExecuteInputTool<ClaireonInputTool_AddActionModifier>(SessionId, Args);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("InputModifierNegate")));

	// Remove modifier at index 0 (DeadZone)
	Args = MakeShared<FJsonObject>();
	Args->SetNumberField(TEXT("index"), 0);
	Result = ExecuteInputTool<ClaireonInputTool_RemoveActionModifier>(SessionId, Args);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("InputModifierNegate")));

	CloseInputSession(SessionId);
	co_return;
}

// ============================================================================
// input_* -- Cross-type Operation Errors
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, IAOperationsOnIMCSession, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId = CreateTestIMC(TEXT("/Game/__MCPTests/IMC_TestCrossType"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// set_value_type on IMC should error
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("value_type"), TEXT("float"));
	auto Result = ExecuteInputTool<ClaireonInputTool_SetValueType>(SessionId, Args);
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Input Action")));

	CloseInputSession(SessionId);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, IMCOperationsOnIASession, UNTEST_TIMEOUTMS(15000))
{
	FString SessionId = CreateTestIA(TEXT("/Game/__MCPTests/IA_TestCrossType2"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// add_mapping on IA should error
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action_path"), TEXT("/Game/__MCPTests/IA_TestCrossType2"));
	Args->SetStringField(TEXT("key"), TEXT("W"));
	auto Result = ExecuteInputTool<ClaireonInputTool_AddMapping>(SessionId, Args);
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Mapping Context")));

	CloseInputSession(SessionId);
	co_return;
}

// ============================================================================
// input_add_mapping / input_set_mapping_key / input_remove_mapping
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, AddRemoveMappings, UNTEST_TIMEOUTMS(20000))
{
	// Create an IA to reference
	FString IASessionId = CreateTestIA(TEXT("/Game/__MCPTests/IA_ForMapping"));
	UNTEST_ASSERT_FALSE(IASessionId.IsEmpty());
	// Save the IA so it can be loaded by the IMC tool
	ExecuteInputTool<ClaireonInputTool_Save>(IASessionId);

	// Create IMC
	FString IMCSessionId = CreateTestIMC(TEXT("/Game/__MCPTests/IMC_TestMappings"));
	UNTEST_ASSERT_FALSE(IMCSessionId.IsEmpty());

	// Add mapping: IA_ForMapping -> W
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action_path"), TEXT("/Game/__MCPTests/IA_ForMapping"));
	Args->SetStringField(TEXT("key"), TEXT("W"));
	auto Result = ExecuteInputTool<ClaireonInputTool_AddMapping>(IMCSessionId, Args);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("IA_ForMapping")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("W")));

	// Add second mapping with SpaceBar
	Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action_path"), TEXT("/Game/__MCPTests/IA_ForMapping"));
	Args->SetStringField(TEXT("key"), TEXT("SpaceBar"));
	Result = ExecuteInputTool<ClaireonInputTool_AddMapping>(IMCSessionId, Args);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("SpaceBar")));

	// Change key on mapping 0 to S
	Args = MakeShared<FJsonObject>();
	Args->SetNumberField(TEXT("index"), 0);
	Args->SetStringField(TEXT("key"), TEXT("S"));
	Result = ExecuteInputTool<ClaireonInputTool_SetMappingKey>(IMCSessionId, Args);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("S")));

	// Remove mapping 0
	Args = MakeShared<FJsonObject>();
	Args->SetNumberField(TEXT("index"), 0);
	Result = ExecuteInputTool<ClaireonInputTool_RemoveMapping>(IMCSessionId, Args);
	// Only SpaceBar mapping should remain
	Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("SpaceBar")));

	CloseInputSession(IMCSessionId);
	CloseInputSession(IASessionId);
	co_return;
}

// ============================================================================
// ClaireonEnhancedInputHelpers -- Unit Tests
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, ParseValueTypes, UNTEST_TIMEOUTMS(5000))
{
	EInputActionValueType Type;
	FString Error;

	UNTEST_EXPECT_TRUE(ClaireonEnhancedInputHelpers::ParseValueType(TEXT("bool"), Type, Error));
	UNTEST_EXPECT_TRUE(Type == EInputActionValueType::Boolean);

	UNTEST_EXPECT_TRUE(ClaireonEnhancedInputHelpers::ParseValueType(TEXT("float"), Type, Error));
	UNTEST_EXPECT_TRUE(Type == EInputActionValueType::Axis1D);

	UNTEST_EXPECT_TRUE(ClaireonEnhancedInputHelpers::ParseValueType(TEXT("2d"), Type, Error));
	UNTEST_EXPECT_TRUE(Type == EInputActionValueType::Axis2D);

	UNTEST_EXPECT_TRUE(ClaireonEnhancedInputHelpers::ParseValueType(TEXT("3d"), Type, Error));
	UNTEST_EXPECT_TRUE(Type == EInputActionValueType::Axis3D);

	UNTEST_EXPECT_FALSE(ClaireonEnhancedInputHelpers::ParseValueType(TEXT("invalid"), Type, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("Unknown value type")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, ResolveTriggerClasses, UNTEST_TIMEOUTMS(5000))
{
	FString Error;

	UClass* DownClass = ClaireonEnhancedInputHelpers::ResolveTriggerClass(TEXT("Down"), Error);
	UNTEST_ASSERT_TRUE(DownClass != nullptr);
	UNTEST_EXPECT_TRUE(DownClass->GetName().Contains(TEXT("Down")));

	UClass* HoldClass = ClaireonEnhancedInputHelpers::ResolveTriggerClass(TEXT("Hold"), Error);
	UNTEST_ASSERT_TRUE(HoldClass != nullptr);
	UNTEST_EXPECT_TRUE(HoldClass->GetName().Contains(TEXT("Hold")));

	UClass* PressedClass = ClaireonEnhancedInputHelpers::ResolveTriggerClass(TEXT("Pressed"), Error);
	UNTEST_ASSERT_TRUE(PressedClass != nullptr);
	UNTEST_EXPECT_TRUE(PressedClass->GetName().Contains(TEXT("Pressed")));

	UClass* InvalidClass = ClaireonEnhancedInputHelpers::ResolveTriggerClass(TEXT("NonexistentTrigger"), Error);
	UNTEST_EXPECT_TRUE(InvalidClass == nullptr);
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("Could not resolve")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, ResolveModifierClasses, UNTEST_TIMEOUTMS(5000))
{
	FString Error;

	UClass* DeadZoneClass = ClaireonEnhancedInputHelpers::ResolveModifierClass(TEXT("DeadZone"), Error);
	UNTEST_ASSERT_TRUE(DeadZoneClass != nullptr);
	UNTEST_EXPECT_TRUE(DeadZoneClass->GetName().Contains(TEXT("DeadZone")));

	UClass* NegateClass = ClaireonEnhancedInputHelpers::ResolveModifierClass(TEXT("Negate"), Error);
	UNTEST_ASSERT_TRUE(NegateClass != nullptr);
	UNTEST_EXPECT_TRUE(NegateClass->GetName().Contains(TEXT("Negate")));

	UClass* InvalidClass = ClaireonEnhancedInputHelpers::ResolveModifierClass(TEXT("NonexistentModifier"), Error);
	UNTEST_EXPECT_TRUE(InvalidClass == nullptr);
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("Could not resolve")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, ResolveKeys, UNTEST_TIMEOUTMS(5000))
{
	FKey Key;
	FString Error;

	UNTEST_EXPECT_TRUE(ClaireonEnhancedInputHelpers::ResolveKey(TEXT("W"), Key, Error));
	UNTEST_EXPECT_TRUE(Key.IsValid());

	UNTEST_EXPECT_TRUE(ClaireonEnhancedInputHelpers::ResolveKey(TEXT("SpaceBar"), Key, Error));
	UNTEST_EXPECT_TRUE(Key.IsValid());

	UNTEST_EXPECT_FALSE(ClaireonEnhancedInputHelpers::ResolveKey(TEXT("NonexistentKey12345"), Key, Error));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("Unknown key")));

	co_return;
}

#endif // WITH_UNTESTED
