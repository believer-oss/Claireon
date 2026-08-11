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
#include "UObject/SoftObjectPath.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"

#include "ClaireonTestAssetDeletion.h"
// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace ClaireonEnhancedInputTests_Private
{
	/** Extract the session ID from an input-edit tool response.
	 *
	 *  Reads the structured Data payload, which is where every input-edit tool puts
	 *  the id (ClaireonInputEditToolBase::BuildStateResponse sets Data->session_id).
	 *
	 *  This used to scan Result.ErrorMessage for a "Session: " marker, which could
	 *  never work: ErrorMessage is only populated by MakeErrorResult, so on the
	 *  SUCCESS path it is always empty and the scan always returned FString(). Every
	 *  test that opened a session therefore failed its first assert
	 *  (SessionId.IsEmpty() should be false) no matter what the tool did. The
	 *  "Session: <id>" line does exist, but in Result.Summary; parsing Data is both
	 *  correct and not formatting-dependent.
	 */
	FString ExtractInputEditSessionId(const IClaireonTool::FToolResult& Result)
	{
		if (Result.bIsError || !Result.Data.IsValid())
		{
			return FString();
		}
		FString SessionId;
		Result.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	/** Delete a fixture asset and its on-disk package.
	 *
	 *  Required BEFORE creating as well as after: /Game/__MCPTests is NOT gitignored and
	 *  survives between runs, and ClaireonInputTool_Create saves the package it
	 *  creates. A fixture left behind by an earlier (or crashed) run makes the next
	 *  'create' fail with "Asset already exists at path ... Use 'open' instead."
	 *  Same pattern as ClaireonListGraphsDataEnvelopeTests.cpp. */
	void CleanupInputAsset(const FString& AssetPath)
	{
		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> AssetsToDelete;
			AssetsToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
		}
		// ForceDeleteObjects can leave the saved package file behind in editor-less
		// runs, so remove it directly and keep /Game/__MCPTests from accumulating.
		const FString PackageFileName = FPackageName::LongPackageNameToFilename(
			AssetPath, FPackageName::GetAssetPackageExtension());
		if (IFileManager::Get().FileExists(*PackageFileName))
		{
			IFileManager::Get().Delete(*PackageFileName, /*RequireExists=*/false, /*EvenReadOnly=*/true);
		}
	}

	/** Create a transient test Input Action via the create tool. Returns session ID. */
	FString CreateTestIA(const FString& Path)
	{
		CleanupInputAsset(Path);
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
		CleanupInputAsset(Path);
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
using namespace ClaireonEnhancedInputTests_Private;

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
	const FString AssetPath = TEXT("/Game/__MCPTests/IA_TestCreate");
	FString SessionId = CreateTestIA(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Status should work
	auto StatusResult = ExecuteInputTool<ClaireonInputTool_Status>(SessionId);
	UNTEST_EXPECT_TRUE(StatusResult.GetContentAsString().Contains(TEXT("Input Action")));

	// Close
	CloseInputSession(SessionId);
	CleanupInputAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, CreateMappingContext, UNTEST_TIMEOUTMS(15000))
{
	const FString AssetPath = TEXT("/Game/__MCPTests/IMC_TestCreate");
	FString SessionId = CreateTestIMC(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	auto StatusResult = ExecuteInputTool<ClaireonInputTool_Status>(SessionId);
	UNTEST_EXPECT_TRUE(StatusResult.GetContentAsString().Contains(TEXT("Input Mapping Context")));

	CloseInputSession(SessionId);
	CleanupInputAsset(AssetPath);
	co_return;
}

/** input_create must never report success with an empty session handle.
 *
 *  FClaireonSessionManager::CanonicalizePath rejects every path outside /Game/,
 *  which is what makes OpenSession return InvalidAssetPath. ClaireonPathResolver
 *  itself accepts /Temp/ (it is a real in-memory mount), so a /Temp/ path sails
 *  through the tool's own path handling and asset creation and only trips at
 *  OpenSession -- the exact combination that used to matter: the tool handled
 *  only BlockedByOtherTool and fell through to BuildStateResponse with an empty
 *  SessionId, handing the caller a SUCCESS response carrying an unusable handle.
 */
UNTEST_UNIT_OPTS(Claireon, EnhancedInput, CreateOutsideGameMountErrorsInsteadOfEmptySession, UNTEST_TIMEOUTMS(15000))
{
	const FString AssetPath = TEXT("/Temp/__MCPTests/IA_OutsideGameMount");
	CleanupInputAsset(AssetPath);

	ClaireonInputTool_Create Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("asset_type"), TEXT("input_action"));
	auto Result = Tool.Execute(Args);

	// The invariant, stated so it holds no matter which non-success result
	// OpenSession picks: a non-error response must carry a usable session id.
	const FString SessionId = ExtractInputEditSessionId(Result);
	UNTEST_EXPECT_FALSE(!Result.bIsError && SessionId.IsEmpty());

	// And the specific behaviour we want: a clear error naming the path.
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Invalid asset path")));

	if (!SessionId.IsEmpty())
	{
		CloseInputSession(SessionId);
	}
	CleanupInputAsset(AssetPath);
	co_return;
}

// ============================================================================
// input_set_value_type
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, SetValueType, UNTEST_TIMEOUTMS(15000))
{
	const FString AssetPath = TEXT("/Game/__MCPTests/IA_TestValueType");
	FString SessionId = CreateTestIA(AssetPath);
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
	CleanupInputAsset(AssetPath);
	co_return;
}

// ============================================================================
// input_add_action_trigger / input_remove_action_trigger
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, AddRemoveActionTriggers, UNTEST_TIMEOUTMS(15000))
{
	const FString AssetPath = TEXT("/Game/__MCPTests/IA_TestTriggers");
	FString SessionId = CreateTestIA(AssetPath);
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
	CleanupInputAsset(AssetPath);
	co_return;
}

// ============================================================================
// input_add_action_modifier / input_remove_action_modifier
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, AddRemoveActionModifiers, UNTEST_TIMEOUTMS(15000))
{
	const FString AssetPath = TEXT("/Game/__MCPTests/IA_TestModifiers");
	FString SessionId = CreateTestIA(AssetPath);
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
	CleanupInputAsset(AssetPath);
	co_return;
}

// ============================================================================
// input_* -- Cross-type Operation Errors
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, IAOperationsOnIMCSession, UNTEST_TIMEOUTMS(15000))
{
	const FString AssetPath = TEXT("/Game/__MCPTests/IMC_TestCrossType");
	FString SessionId = CreateTestIMC(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// set_value_type on IMC should error
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("value_type"), TEXT("float"));
	auto Result = ExecuteInputTool<ClaireonInputTool_SetValueType>(SessionId, Args);
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Input Action")));

	CloseInputSession(SessionId);
	CleanupInputAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, IMCOperationsOnIASession, UNTEST_TIMEOUTMS(15000))
{
	const FString AssetPath = TEXT("/Game/__MCPTests/IA_TestCrossType2");
	FString SessionId = CreateTestIA(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// add_mapping on IA should error
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action_path"), AssetPath);
	Args->SetStringField(TEXT("key"), TEXT("W"));
	auto Result = ExecuteInputTool<ClaireonInputTool_AddMapping>(SessionId, Args);
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Mapping Context")));

	CloseInputSession(SessionId);
	CleanupInputAsset(AssetPath);
	co_return;
}

// ============================================================================
// input_add_mapping / input_set_mapping_key / input_remove_mapping
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EnhancedInput, AddRemoveMappings, UNTEST_TIMEOUTMS(20000))
{
	// Create an IA to reference
	const FString IAPath = TEXT("/Game/__MCPTests/IA_ForMapping");
	const FString IMCPath = TEXT("/Game/__MCPTests/IMC_TestMappings");
	FString IASessionId = CreateTestIA(IAPath);
	UNTEST_ASSERT_FALSE(IASessionId.IsEmpty());
	// Save the IA so it can be loaded by the IMC tool
	ExecuteInputTool<ClaireonInputTool_Save>(IASessionId);

	// Create IMC
	FString IMCSessionId = CreateTestIMC(IMCPath);
	UNTEST_ASSERT_FALSE(IMCSessionId.IsEmpty());

	// Add mapping: IA_ForMapping -> W
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action_path"), IAPath);
	Args->SetStringField(TEXT("key"), TEXT("W"));
	auto Result = ExecuteInputTool<ClaireonInputTool_AddMapping>(IMCSessionId, Args);
	FString Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("IA_ForMapping")));
	// Match the formatted mapping line ("| Key: W\n"), not a bare "W": a bare
	// single-character Contains() is satisfied by unrelated text in the state
	// dump and so asserts nothing.
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("Key: W\n")));

	// Add second mapping with SpaceBar
	Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("action_path"), IAPath);
	Args->SetStringField(TEXT("key"), TEXT("SpaceBar"));
	Result = ExecuteInputTool<ClaireonInputTool_AddMapping>(IMCSessionId, Args);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("SpaceBar")));

	// Change key on mapping 0 to S
	Args = MakeShared<FJsonObject>();
	Args->SetNumberField(TEXT("index"), 0);
	Args->SetStringField(TEXT("key"), TEXT("S"));
	Result = ExecuteInputTool<ClaireonInputTool_SetMappingKey>(IMCSessionId, Args);
	// Same reason as above: "Key: S\n" pins the rebound mapping line and cannot be
	// satisfied by the "SpaceBar" mapping or by surrounding prose.
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Key: S\n")));

	// Remove mapping 0
	Args = MakeShared<FJsonObject>();
	Args->SetNumberField(TEXT("index"), 0);
	Result = ExecuteInputTool<ClaireonInputTool_RemoveMapping>(IMCSessionId, Args);
	// Only SpaceBar mapping should remain
	Output = Result.GetContentAsString();
	UNTEST_EXPECT_TRUE(Output.Contains(TEXT("SpaceBar")));

	CloseInputSession(IMCSessionId);
	CloseInputSession(IASessionId);
	// IMC first: it references the IA, so deleting the referencer first avoids
	// ForceDeleteObjects having to null out a live reference.
	CleanupInputAsset(IMCPath);
	CleanupInputAsset(IAPath);
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
