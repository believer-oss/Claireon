// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
//
// P1-8 and P1-9: writes that reported success without doing what was asked.
//
//   P1-8   bp_set_property rejected every EditDefaultsOnly property (the gate
//          was CPF_DisableEditOnInstance, which IS EditDefaultsOnly) and its
//          write discarded ImportText_Direct's return value.
//   P1-8   bp_set_cdo_property concatenates property_path + '.' + property_name;
//          the overlapping form leaked "Cannot navigate through non-struct".
//   P1-9a  add_function_override.interface_class was declared and never read.
//   P1-9b  bp_add_function declared inputs/outputs as objects but read arrays,
//          and dropped malformed entries with a `continue`.
//   P1-9e  bp_remove_variable force=true skipped the referrer scan, so the
//          caller was never told which nodes it had just broken.
//
//   P1-9c  bp_apply_spec wrote pin defaults with an unchecked
//          TrySetDefaultValue, so a rejected literal was reported as applied.
//   P1-9d  the pinless-node guard covered UK2Node_Variable only, so a
//          FunctionReference that failed during AllocateDefaultPins committed a
//          dead zero-pin node with a success message.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonSessionManager.h"
#include "ClaireonStructReflection.h"
#include "ClaireonTestAssetDeletion.h"
#include "ClaireonTestTypes.h"
#include "Tools/ClaireonBlueprintGraphTool_AddFunction.h"
#include "Tools/ClaireonBlueprintGraphTool_AddFunctionOverride.h"
#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/ClaireonBlueprintGraphTool_AddVariable.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_RemoveVariable.h"
#include "Tools/ClaireonBlueprintGraphTool_SetProperty.h"
#include "Tools/ClaireonTool_SetBlueprintCDOProperty.h"
#include "Tools/IClaireonTool.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"

// File-local namespace (NOT raw `namespace { ... }`) to avoid unity-batched
// symbol collisions across other Tests TUs.
namespace ClaireonSilentWriteTests
{
static void SSW_CleanupAsset(const FString& AssetPath)
{
	FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);

	const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
	if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
	{
		TArray<UObject*> AssetsToDelete;
		AssetsToDelete.Add(Asset);
		ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
	}
}

/** Create a Blueprint parented to ParentClass and return its session id. */
static FString SSW_CreateAndOpen(const TCHAR* AssetPath, const TCHAR* ParentClass)
{
	ClaireonBlueprintGraphTool_Create CreateTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("parent_class"), ParentClass);
	const IClaireonTool::FToolResult R = CreateTool.Execute(Args);
	if (R.bIsError || !R.Data.IsValid())
	{
		return FString();
	}
	FString SessionId;
	R.Data->TryGetStringField(TEXT("session_id"), SessionId);
	return SessionId;
}

static IClaireonTool::FToolResult SSW_SetProperty(
	const FString& SessionId, const TCHAR* Name, const TCHAR* Value, bool bAllowNonEditable = false)
{
	ClaireonBlueprintGraphTool_SetProperty Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("property_name"), Name);
	Args->SetStringField(TEXT("property_value"), Value);
	if (bAllowNonEditable)
	{
		Args->SetBoolField(TEXT("allow_non_editable"), true);
	}
	return Tool.Execute(Args);
}
} // namespace ClaireonSilentWriteTests

// ---------------------------------------------------------------------------
// P1-8: the gate.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, SilentWrite, EditDefaultsOnlyPropertyIsWritable, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonSilentWriteTests;

	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_SSW_EditDefaultsOnly");
	SSW_CleanupAsset(AssetPath);

	const FString SessionId = SSW_CreateAndOpen(AssetPath, TEXT("ClaireonEditabilityFixtureActor"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// The old gate rejected this outright: EditDefaultsOnly sets
	// CPF_DisableEditOnInstance, and the gate refused anything carrying it.
	const IClaireonTool::FToolResult R = SSW_SetProperty(SessionId, TEXT("bEditDefaultsOnlyFlag"), TEXT("true"));
	const bool bWasError = R.bIsError;
	const FString ErrorText = R.ErrorMessage;

	// Read back through the CDO rather than trusting the report.
	bool bLandedValue = false;
	{
		const FString ObjectPath = FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).TryLoad());
			IsValid(BP) && IsValid(BP->GeneratedClass))
		{
			if (const AClaireonEditabilityFixtureActor* CDO =
					Cast<AClaireonEditabilityFixtureActor>(BP->GeneratedClass->GetDefaultObject()))
			{
				bLandedValue = CDO->bEditDefaultsOnlyFlag;
			}
		}
	}

	SSW_CleanupAsset(AssetPath);

	UNTEST_ASSERT_FALSE(bWasError);
	UNTEST_ASSERT_TRUE(bLandedValue);
	// The gate and uobject_inspect's reported editor_access share one function,
	// so they cannot disagree; this pins the value that function returns.
	UNTEST_EXPECT_STREQ(
		*ClaireonStructReflection::DescribeEditorAccess(
			AClaireonEditabilityFixtureActor::StaticClass()
				->FindPropertyByName(TEXT("bEditDefaultsOnlyFlag"))
				->GetPropertyFlags()),
		TEXT("edit"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SilentWrite, VisibleAnywhereRefusesAndNamesTheEscapeHatch, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonSilentWriteTests;

	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_SSW_VisibleOnly");
	SSW_CleanupAsset(AssetPath);

	const FString SessionId = SSW_CreateAndOpen(AssetPath, TEXT("ClaireonEditabilityFixtureActor"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const IClaireonTool::FToolResult Refused = SSW_SetProperty(SessionId, TEXT("VisibleOnlyNumber"), TEXT("7"));
	const bool bRefused = Refused.bIsError;
	const FString RefusalText = Refused.ErrorMessage;

	// With the flag it goes through, so the refusal is a gate and not a wall.
	const IClaireonTool::FToolResult Allowed =
		SSW_SetProperty(SessionId, TEXT("VisibleOnlyNumber"), TEXT("7"), /*bAllowNonEditable=*/true);
	const bool bAllowedError = Allowed.bIsError;

	SSW_CleanupAsset(AssetPath);

	UNTEST_ASSERT_TRUE(bRefused);
	UNTEST_ASSERT_TRUE(RefusalText.Contains(TEXT("allow_non_editable")));
	UNTEST_ASSERT_FALSE(bAllowedError);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SilentWrite, UnparseableValueErrorsInsteadOfSucceeding, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonSilentWriteTests;

	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_SSW_BadValue");
	SSW_CleanupAsset(AssetPath);

	const FString SessionId = SSW_CreateAndOpen(AssetPath, TEXT("ClaireonEditabilityFixtureActor"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// The bare ImportText_Direct discarded its return value, so this reported success.
	const IClaireonTool::FToolResult R =
		SSW_SetProperty(SessionId, TEXT("EditAnywhereNumber"), TEXT("not-a-number"));
	const bool bWasError = R.bIsError;

	SSW_CleanupAsset(AssetPath);

	UNTEST_ASSERT_TRUE(bWasError);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SilentWrite, CdoPropertyPathOverlapIsNamed, UNTEST_TIMEOUTMS(30000))
{
	// No asset needed: the overlap is an argument-shape error and is answered
	// before anything is loaded. An unsaved in-memory Blueprint would fail the
	// LoadObject step first and never reach the guard, which is what made an
	// asset-based version of this test measure the wrong thing.
	ClaireonTool_SetBlueprintCDOProperty Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_SSW_NoSuchAsset"));
	Args->SetStringField(TEXT("property_path"), TEXT("PrimaryActorTick.bCanEverTick"));
	Args->SetStringField(TEXT("property_name"), TEXT("bCanEverTick"));
	Args->SetStringField(TEXT("value"), TEXT("true"));
	const IClaireonTool::FToolResult R = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(R.bIsError);
	// Must name the split contract, not leak "Cannot navigate through non-struct"
	// and not report the (also nonexistent) asset -- the arguments are wrong
	// whatever the asset is.
	UNTEST_ASSERT_TRUE(R.ErrorMessage.Contains(TEXT("property_path")));
	UNTEST_ASSERT_TRUE(R.ErrorMessage.Contains(TEXT("property_name")));
	co_return;
}

// ---------------------------------------------------------------------------
// P1-9b: bp_add_function inputs/outputs.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, SilentWrite, AddFunctionRejectsDictOutputs, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonSilentWriteTests;

	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_SSW_AddFuncDict");
	SSW_CleanupAsset(AssetPath);

	const FString SessionId = SSW_CreateAndOpen(AssetPath, TEXT("Actor"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// The schema said `object`, so this shape was the documented one -- and the
	// code, which reads arrays, skipped it and returned a pinless function.
	ClaireonBlueprintGraphTool_AddFunction Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("function_name"), TEXT("ClaireonDictOutputs"));
	TSharedPtr<FJsonObject> DictOutputs = MakeShared<FJsonObject>();
	DictOutputs->SetStringField(TEXT("ReturnValue"), TEXT("bool"));
	Args->SetObjectField(TEXT("outputs"), DictOutputs);

	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	const bool bWasError = R.bIsError;
	const FString ErrorText = R.ErrorMessage;

	SSW_CleanupAsset(AssetPath);

	UNTEST_ASSERT_TRUE(bWasError);
	UNTEST_ASSERT_TRUE(ErrorText.Contains(TEXT("outputs")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, SilentWrite, AddFunctionRejectsEntryWithoutType, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonSilentWriteTests;

	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_SSW_AddFuncNoType");
	SSW_CleanupAsset(AssetPath);

	const FString SessionId = SSW_CreateAndOpen(AssetPath, TEXT("Actor"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	ClaireonBlueprintGraphTool_AddFunction Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("function_name"), TEXT("ClaireonNoTypeEntry"));

	TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("name"), TEXT("R"));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(Entry));
	Args->SetArrayField(TEXT("outputs"), Outputs);

	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	const bool bWasError = R.bIsError;
	const FString ErrorText = R.ErrorMessage;

	SSW_CleanupAsset(AssetPath);

	// Previously a per-entry `continue`: the entry vanished and the function was
	// reported as created with no output pin.
	UNTEST_ASSERT_TRUE(bWasError);
	UNTEST_ASSERT_TRUE(ErrorText.Contains(TEXT("type")));
	co_return;
}

// ---------------------------------------------------------------------------
// P1-9a: add_function_override.interface_class.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, SilentWrite, FunctionOverrideNamesInterfaceClass, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonSilentWriteTests;

	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_SSW_IfaceClass");
	SSW_CleanupAsset(AssetPath);

	const FString SessionId = SSW_CreateAndOpen(AssetPath, TEXT("Actor"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	ClaireonBlueprintGraphTool_AddFunctionOverride Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("function_name"), TEXT("SomeInterfaceFunction"));
	Args->SetStringField(TEXT("interface_class"), TEXT("BlueprintFunctionLibrary"));

	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	const bool bWasError = R.bIsError;
	const FString ErrorText = R.ErrorMessage;

	SSW_CleanupAsset(AssetPath);

	// The parameter was accepted and never read: the call fell through to a
	// parent-class-only lookup whose error never mentioned the interface.
	UNTEST_ASSERT_TRUE(bWasError);
	UNTEST_ASSERT_TRUE(ErrorText.Contains(TEXT("interface_class")));
	co_return;
}

// ---------------------------------------------------------------------------
// P1-9e: bp_remove_variable force=true.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, SilentWrite, ForcedRemoveVariableListsBrokenReferrers, UNTEST_TIMEOUTMS(90000))
{
	using namespace ClaireonSilentWriteTests;

	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_SSW_ForceRemoveVar");
	SSW_CleanupAsset(AssetPath);

	const FString SessionId = SSW_CreateAndOpen(AssetPath, TEXT("Actor"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// A variable plus two Get nodes referencing it.
	{
		ClaireonBlueprintGraphTool_AddVariable AddVar;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("variable_name"), TEXT("ClaireonForcedVar"));
		Args->SetStringField(TEXT("variable_type"), TEXT("bool"));
		const IClaireonTool::FToolResult R = AddVar.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}

	int32 GetNodesCreated = 0;
	for (int32 i = 0; i < 2; ++i)
	{
		ClaireonBlueprintGraphTool_AddNode AddNode;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("VariableGet"));
		Args->SetStringField(TEXT("variable_name"), TEXT("ClaireonForcedVar"));
		Args->SetNumberField(TEXT("position_x"), 100 + i * 250);
		Args->SetNumberField(TEXT("position_y"), 400);
		const IClaireonTool::FToolResult R = AddNode.Execute(Args);
		if (!R.bIsError)
		{
			++GetNodesCreated;
		}
	}

	ClaireonBlueprintGraphTool_RemoveVariable RemoveVar;
	TSharedPtr<FJsonObject> ForceArgs = MakeShared<FJsonObject>();
	ForceArgs->SetStringField(TEXT("session_id"), SessionId);
	ForceArgs->SetStringField(TEXT("variable_name"), TEXT("ClaireonForcedVar"));
	ForceArgs->SetBoolField(TEXT("force"), true);
	const IClaireonTool::FToolResult R = RemoveVar.Execute(ForceArgs);

	const bool bWasError = R.bIsError;
	int32 ReportedReferrers = 0;
	if (R.Data.IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* Broken = nullptr;
		if (R.Data->TryGetArrayField(TEXT("broken_referrers"), Broken) && Broken != nullptr)
		{
			ReportedReferrers = Broken->Num();
		}
	}

	SSW_CleanupAsset(AssetPath);

	UNTEST_ASSERT_EQ(GetNodesCreated, 2);
	// force means "remove it anyway", not "do not tell me what broke": the scan
	// used to be skipped entirely and the success said only "scan skipped".
	UNTEST_ASSERT_FALSE(bWasError);
	UNTEST_ASSERT_EQ(ReportedReferrers, 2);
	co_return;
}

// ---------------------------------------------------------------------------
// P1-9c (bp_apply_spec pin_defaults post-write compare) is NOT pinned by a test
// here, deliberately, and the reason is worth recording.
//
// The fix adds a post-write compare after TrySetDefaultValue, which swallows
// validation failures. To exercise it a test must get the schema to REJECT a
// literal -- and on this 5.5 engine build
// UEdGraphSchema_K2::TrySetDefaultValue stores "not-a-float" on a float pin
// instead of rejecting it. Three attempts through bp_apply_spec produced
// neither a rejection nor a disclosure, so the test was measuring the engine's
// leniency rather than Claireon's reporting.
//
// The fix itself is a verbatim port of the compare in
// ClaireonTool_ApplyBlueprintDelta.cpp's pin_defaults arm, which IS covered.
// A future test wants a pin type whose validation is strict on this build
// (start with an enum or byte pin and an invalid name), not another float.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// P1-9d: the pinless-node guard is now generalized from UK2Node_Variable to
// UK2Node_CallFunction. The risk of that widening is a FALSE POSITIVE deleting
// working nodes, so this pins the negative: an ordinary call node still lands.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, SilentWrite, ValidCallNodeSurvivesThePinlessGuard, UNTEST_TIMEOUTMS(90000))
{
	using namespace ClaireonSilentWriteTests;

	const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_SSW_CallNodeGuard");
	SSW_CleanupAsset(AssetPath);

	const FString SessionId = SSW_CreateAndOpen(AssetPath, TEXT("Actor"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	ClaireonBlueprintGraphTool_AddNode AddNode;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	Args->SetStringField(TEXT("function_name"), TEXT("PrintString"));
	Args->SetNumberField(TEXT("position_x"), 200);
	Args->SetNumberField(TEXT("position_y"), 200);
	const IClaireonTool::FToolResult R = AddNode.Execute(Args);

	const bool bWasError = R.bIsError;
	const FString ErrorText = R.ErrorMessage;

	SSW_CleanupAsset(AssetPath);

	// The guard must not have fired. Check that first: it is the specific
	// regression this test exists for, and a bare bWasError failure would not
	// say which of the two happened.
	UNTEST_ASSERT_FALSE(ErrorText.Contains(TEXT("did not bind after AllocateDefaultPins")));
	UNTEST_ASSERT_FALSE(bWasError);
	co_return;
}

#endif // WITH_UNTESTED
