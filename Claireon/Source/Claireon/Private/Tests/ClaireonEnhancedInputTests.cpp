// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonTool_InputInspect.h"
#include "Tools/ClaireonTool_InputEdit.h"
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

	/** Create a transient test Input Action via the edit tool. Returns session ID. */
	FString CreateTestIA(ClaireonTool_InputEdit& Tool, const FString& Path)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("create"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Path);
		Params->SetStringField(TEXT("asset_type"), TEXT("input_action"));
		Args->SetObjectField(TEXT("params"), Params);
		auto Result = Tool.Execute(Args);
		return ExtractInputEditSessionId(Result);
	}

	/** Create a transient test IMC via the edit tool. Returns session ID. */
	FString CreateTestIMC(ClaireonTool_InputEdit& Tool, const FString& Path)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("create"));
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), Path);
		Params->SetStringField(TEXT("asset_type"), TEXT("mapping_context"));
		Args->SetObjectField(TEXT("params"), Params);
		auto Result = Tool.Execute(Args);
		return ExtractInputEditSessionId(Result);
	}

	/** Execute an edit operation with session ID and params. */
	IClaireonTool::FToolResult EditOp(ClaireonTool_InputEdit& Tool, const FString& Op, const FString& SessionId, TSharedPtr<FJsonObject> Params = nullptr)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), Op);
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetObjectField(TEXT("params"), Params ? Params : MakeShared<FJsonObject>());
		return Tool.Execute(Args);
	}

	/** Close a session. */
	void CloseSession(ClaireonTool_InputEdit& Tool, const FString& SessionId)
	{
		EditOp(Tool, TEXT("close"), SessionId);
	}
}

// ============================================================================
// claireon.input_inspect
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
// claireon.input_edit -- Error Handling
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, EditMissingOperation, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_InputEdit Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("operation")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, EditMissingSessionId, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_InputEdit Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("operation"), TEXT("status"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("session_id")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, EditInvalidSessionId, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_InputEdit Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("operation"), TEXT("status"));
	Args->SetStringField(TEXT("session_id"), TEXT("nonexistent-session-id"));
	Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("not found")));
	co_return;
}

// ============================================================================
// claireon.input_edit -- Create + Session Lifecycle
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, CreateInputAction, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_InputEdit Tool;
	FString SessionId = CreateTestIA(Tool, TEXT("/Game/__MCPTests/IA_TestCreate"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Status should work
	auto StatusResult = EditOp(Tool, TEXT("status"), SessionId);
	UNTEST_EXPECT_TRUE(StatusResult.GetContentAsString().Contains(TEXT("Input Action")));

	// Close
	CloseSession(Tool, SessionId);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, CreateMappingContext, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_InputEdit Tool;
	FString SessionId = CreateTestIMC(Tool, TEXT("/Game/__MCPTests/IMC_TestCreate"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	auto StatusResult = EditOp(Tool, TEXT("status"), SessionId);
	UNTEST_EXPECT_TRUE(StatusResult.GetContentAsString().Contains(TEXT("Input Mapping Context")));

	CloseSession(Tool, SessionId);
	co_return;
}

// ============================================================================
// claireon.input_edit -- Input Action Value Type
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, SetValueType, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_InputEdit Tool;
	FString SessionId = CreateTestIA(Tool, TEXT("/Game/__MCPTests/IA_TestValueType"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Set to Axis2D
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("value_type"), TEXT("2d"));
	auto Result = EditOp(Tool, TEXT("set_value_type"), SessionId, Params);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Axis2D")));

	// Set to Boolean
	Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("value_type"), TEXT("bool"));
	Result = EditOp(Tool, TEXT("set_value_type"), SessionId, Params);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Boolean")));

	CloseSession(Tool, SessionId);
	co_return;
}

// ============================================================================
// claireon.input_edit -- Input Action Triggers
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, AddRemoveActionTriggers, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_InputEdit Tool;
	FString SessionId = CreateTestIA(Tool, TEXT("/Game/__MCPTests/IA_TestTriggers"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Add Down trigger
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("trigger_class"), TEXT("Down"));
	auto Result = EditOp(Tool, TEXT("add_action_trigger"), SessionId, Params);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("InputTriggerDown")));

	// Add Hold trigger
	Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("trigger_class"), TEXT("Hold"));
	Result = EditOp(Tool, TEXT("add_action_trigger"), SessionId, Params);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("InputTriggerHold")));

	// Remove trigger at index 0 (Down)
	Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("index"), 0);
	Result = EditOp(Tool, TEXT("remove_action_trigger"), SessionId, Params);
	// After removal, only Hold should remain
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("InputTriggerHold")));

	// Out of bounds index should error
	Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("index"), 99);
	Result = EditOp(Tool, TEXT("remove_action_trigger"), SessionId, Params);
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("out of range")));

	CloseSession(Tool, SessionId);
	co_return;
}

// ============================================================================
// claireon.input_edit -- Input Action Modifiers
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, AddRemoveActionModifiers, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_InputEdit Tool;
	FString SessionId = CreateTestIA(Tool, TEXT("/Game/__MCPTests/IA_TestModifiers"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Add DeadZone modifier
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("modifier_class"), TEXT("DeadZone"));
	auto Result = EditOp(Tool, TEXT("add_action_modifier"), SessionId, Params);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("InputModifierDeadZone")));

	// Add Negate modifier
	Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("modifier_class"), TEXT("Negate"));
	Result = EditOp(Tool, TEXT("add_action_modifier"), SessionId, Params);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("InputModifierNegate")));

	// Remove modifier at index 0 (DeadZone)
	Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("index"), 0);
	Result = EditOp(Tool, TEXT("remove_action_modifier"), SessionId, Params);
	// Only Negate should remain
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("InputModifierNegate")));

	CloseSession(Tool, SessionId);
	co_return;
}

// ============================================================================
// claireon.input_edit -- Cross-type Operation Errors
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, IAOperationsOnIMCSession, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_InputEdit Tool;
	FString SessionId = CreateTestIMC(Tool, TEXT("/Game/__MCPTests/IMC_TestCrossType"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// set_value_type on IMC should error
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("value_type"), TEXT("float"));
	auto Result = EditOp(Tool, TEXT("set_value_type"), SessionId, Params);
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Input Action")));

	CloseSession(Tool, SessionId);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, IMCOperationsOnIASession, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_InputEdit Tool;
	FString SessionId = CreateTestIA(Tool, TEXT("/Game/__MCPTests/IA_TestCrossType2"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// add_mapping on IA should error
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action_path"), TEXT("/Game/__MCPTests/IA_TestCrossType2"));
	Params->SetStringField(TEXT("key"), TEXT("W"));
	auto Result = EditOp(Tool, TEXT("add_mapping"), SessionId, Params);
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Mapping Context")));

	CloseSession(Tool, SessionId);
	co_return;
}

// ============================================================================
// claireon.input_edit -- IMC Mappings
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, AddRemoveMappings, UNTEST_TIMEOUTMS(20000))
{
	ClaireonTool_InputEdit Tool;

	// Create an IA to reference
	FString IASessionId = CreateTestIA(Tool, TEXT("/Game/__MCPTests/IA_ForMapping"));
	UNTEST_ASSERT_FALSE(IASessionId.IsEmpty());
	// Save the IA so it can be loaded by the IMC tool
	EditOp(Tool, TEXT("save"), IASessionId);

	// Create IMC
	FString IMCSessionId = CreateTestIMC(Tool, TEXT("/Game/__MCPTests/IMC_TestMappings"));
	UNTEST_ASSERT_FALSE(IMCSessionId.IsEmpty());

	// Add mapping: IA_ForMapping -> W
	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action_path"), TEXT("/Game/__MCPTests/IA_ForMapping"));
	Params->SetStringField(TEXT("key"), TEXT("W"));
	auto Result = EditOp(Tool, TEXT("add_mapping"), IMCSessionId, Params);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("IA_ForMapping")));
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("W")));

	// Add second mapping with SpaceBar
	Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("action_path"), TEXT("/Game/__MCPTests/IA_ForMapping"));
	Params->SetStringField(TEXT("key"), TEXT("SpaceBar"));
	Result = EditOp(Tool, TEXT("add_mapping"), IMCSessionId, Params);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("SpaceBar")));

	// Change key on mapping 0 to S
	Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("index"), 0);
	Params->SetStringField(TEXT("key"), TEXT("S"));
	Result = EditOp(Tool, TEXT("set_mapping_key"), IMCSessionId, Params);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("S")));

	// Remove mapping 0
	Params = MakeShared<FJsonObject>();
	Params->SetNumberField(TEXT("index"), 0);
	Result = EditOp(Tool, TEXT("remove_mapping"), IMCSessionId, Params);
	// Only SpaceBar mapping should remain
	Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("SpaceBar")));

	CloseSession(Tool, IMCSessionId);
	CloseSession(Tool, IASessionId);
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
