// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Behavioural tests for the RepNotify handler auto-creation feature shared by
// bp_add_variable and bp_set_variable_properties.
//
// Covers five scenarios:
//  1. Default-name RepNotify creates OnRep_<VarName> and surfaces rep_notify_graph.
//  2. Custom rep_notify_func overrides the default handler name.
//  3. Idempotent re-invocation: repeat calls reuse the graph (pointer identity preserved).
//  4. snake_case replication='rep_notify' alias behaves identically to PascalCase.
//  5. Non-RepNotify replication modes do NOT surface rep_notify_graph.
//
// Tests create throwaway blueprints under /Game/__MCPTests/, invoke tools via
// the asset_path auto-open path, assert expected state, and clean up.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonBlueprintGraphTool_AddVariable.h"
#include "Tools/ClaireonBlueprintGraphTool_SetVariableProperties.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/IClaireonTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonTool_AddVariableRepNotifyTests_Private
{

static const TCHAR* TestBPPath_Default    = TEXT("/Game/__MCPTests/BP_RepNotify_Default");
static const TCHAR* TestBPPath_Custom     = TEXT("/Game/__MCPTests/BP_RepNotify_Custom");
static const TCHAR* TestBPPath_Idempotent = TEXT("/Game/__MCPTests/BP_RepNotify_Idempotent");
static const TCHAR* TestBPPath_Snake      = TEXT("/Game/__MCPTests/BP_RepNotify_Snake");
static const TCHAR* TestBPPath_NoGraph    = TEXT("/Game/__MCPTests/BP_RepNotify_NoGraph");

// True only when the fixture actually has a .uasset on disk.
//
// The fixture below is built in an in-memory package and deliberately NOT saved:
// nothing in this suite reads the asset back from disk, and neither
// bp_add_variable nor bp_set_variable_properties saves (only bp_save /
// bp_close_all reach CompileAndSaveSession, and these tests never call them).
// Deleting an in-memory fixture buys nothing, and every
// ObjectTools::ForceDeleteObjects call runs a whole-object-graph referencer scan,
// which is the trigger for the nondeterministic Niagara-serialization crash
// documented in Docs/llm/todo/claireon-untest-harness-reliability.md item 1.
//
// The check is kept rather than dropping the delete outright because
// /Game/__MCPTests is deliberately NOT gitignored: a stale .uasset left by an
// older build (this helper's fixture used to be saved with UPackage::Save) or a
// crashed run must still be cleaned so `git status --porcelain -- Content/` stays
// empty.
bool RepNotifyTests_HasFileOnDisk(const FString& AssetOrPackagePath)
{
	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetOrPackagePath);
	FString FileName;
	if (!FPackageName::TryConvertLongPackageNameToFilename(
			PackageName, FileName, FPackageName::GetAssetPackageExtension()))
	{
		return false;
	}
	return FPaths::FileExists(FileName);
}

// Delete the asset at AssetPath if it exists on disk.
void CleanupBlueprintAsset(const FString& AssetPath)
{
	// In-memory fixture: nothing on disk, nothing to clean, no referencer scan.
	if (!RepNotifyTests_HasFileOnDisk(AssetPath))
	{
		return;
	}

	const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
	if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
	{
		TArray<UObject*> AssetsToDelete;
		AssetsToDelete.Add(Asset);
		ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
	}
}

// Create a pristine plain AActor-parented Blueprint in an in-memory package at
// AssetPath. Evicting whatever occupies the name instead of reusing or deleting it
// is the same idiom bp_create uses (ClaireonBlueprintHelpers::CreateBlueprint) and,
// unlike a delete, it runs no referencer scan.
UBlueprint* CreatePlainActorBlueprint(const FString& AssetPath)
{
	UPackage* Package = CreatePackage(*AssetPath);
	if (!IsValid(Package))
	{
		return nullptr;
	}

	const FString AssetName = FPackageName::GetShortName(AssetPath);
	ClaireonAssetUtils::EvictInMemoryObject(Package, AssetName);

	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		Package,
		FName(*AssetName),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None);

	if (!IsValid(BP))
	{
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(BP);
	BP->MarkPackageDirty();

	return BP;
}

// Look up the FBPVariableDescription for VarName on Blueprint, or nullptr.
const FBPVariableDescription* FindVarDesc(UBlueprint* Blueprint, const TCHAR* VarName)
{
	if (!IsValid(Blueprint))
	{
		return nullptr;
	}
	const FName VarFName(VarName);
	for (const FBPVariableDescription& Var : Blueprint->NewVariables)
	{
		if (Var.VarName == VarFName)
		{
			return &Var;
		}
	}
	return nullptr;
}

// Return true if Blueprint->FunctionGraphs contains a graph with the given name.
bool HasFunctionGraph(UBlueprint* Blueprint, const TCHAR* GraphName)
{
	if (!IsValid(Blueprint))
	{
		return false;
	}
	const FName GraphFName(GraphName);
	for (const UEdGraph* G : Blueprint->FunctionGraphs)
	{
		if (IsValid(G) && G->GetFName() == GraphFName)
		{
			return true;
		}
	}
	return false;
}

// Find the UEdGraph named GraphName in Blueprint->FunctionGraphs, or nullptr.
UEdGraph* FindFunctionGraph(UBlueprint* Blueprint, const TCHAR* GraphName)
{
	if (!IsValid(Blueprint))
	{
		return nullptr;
	}
	const FName GraphFName(GraphName);
	for (UEdGraph* G : Blueprint->FunctionGraphs)
	{
		if (IsValid(G) && G->GetFName() == GraphFName)
		{
			return G;
		}
	}
	return nullptr;
}

// Build AddVariable args with asset_path auto-open.
TSharedPtr<FJsonObject> MakeAddVarArgs(const TCHAR* AssetPath, const TCHAR* VarName,
	const TCHAR* VarType, const TCHAR* Replication, const TCHAR* RepNotifyFunc = nullptr)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("variable_name"), VarName);
	Args->SetStringField(TEXT("variable_type"), VarType);
	if (Replication)
	{
		Args->SetStringField(TEXT("replication"), Replication);
	}
	if (RepNotifyFunc)
	{
		Args->SetStringField(TEXT("rep_notify_func"), RepNotifyFunc);
	}
	return Args;
}

// Build SetVariableProperties args with asset_path auto-open.
TSharedPtr<FJsonObject> MakeSetVarPropsArgs(const TCHAR* AssetPath, const TCHAR* VarName,
	const TCHAR* Replication)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("variable_name"), VarName);
	if (Replication)
	{
		Args->SetStringField(TEXT("replication"), Replication);
	}
	return Args;
}

} // namespace ClaireonTool_AddVariableRepNotifyTests_Private
using namespace ClaireonTool_AddVariableRepNotifyTests_Private;

// ============================================================================
// Test 1: AddVariable creates default OnRep_<Name> handler
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, AddVarRepNotify, Functional_DefaultHandlerNameCreatesOnRepGraph, UNTEST_TIMEOUTMS(60000))
{
	CleanupBlueprintAsset(TestBPPath_Default);

	UBlueprint* BP = CreatePlainActorBlueprint(TestBPPath_Default);
	UNTEST_ASSERT_PTR(BP);

	ClaireonBlueprintGraphTool_AddVariable AddVarTool;
	IClaireonTool::FToolResult Result = AddVarTool.Execute(
		MakeAddVarArgs(TestBPPath_Default, TEXT("Target"), TEXT("int"), TEXT("RepNotify")));

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	// data.rep_notify_graph == "OnRep_Target"
	FString RepNotifyGraph;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetStringField(TEXT("rep_notify_graph"), RepNotifyGraph));
	UNTEST_EXPECT_STREQ(*RepNotifyGraph, TEXT("OnRep_Target"));

	// Function graph exists on the blueprint
	UNTEST_EXPECT_TRUE(HasFunctionGraph(BP, TEXT("OnRep_Target")));

	// Variable has RepNotifyFunc = OnRep_Target and Net|RepNotify flags set
	const FBPVariableDescription* VarDesc = FindVarDesc(BP, TEXT("Target"));
	UNTEST_ASSERT_PTR(VarDesc);
	UNTEST_EXPECT_STREQ(*VarDesc->RepNotifyFunc.ToString(), TEXT("OnRep_Target"));
	const uint64 RepFlags = VarDesc->PropertyFlags & (CPF_Net | CPF_RepNotify);
	UNTEST_EXPECT_TRUE(RepFlags == (CPF_Net | CPF_RepNotify));

	CleanupBlueprintAsset(TestBPPath_Default);
	co_return;
}

// ============================================================================
// Test 2: AddVariable honours custom rep_notify_func
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, AddVarRepNotify, Functional_CustomRepNotifyFuncNamesGraph, UNTEST_TIMEOUTMS(60000))
{
	CleanupBlueprintAsset(TestBPPath_Custom);

	UBlueprint* BP = CreatePlainActorBlueprint(TestBPPath_Custom);
	UNTEST_ASSERT_PTR(BP);

	ClaireonBlueprintGraphTool_AddVariable AddVarTool;
	IClaireonTool::FToolResult Result = AddVarTool.Execute(
		MakeAddVarArgs(TestBPPath_Custom, TEXT("CustomVar"), TEXT("int"), TEXT("RepNotify"), TEXT("Custom_OnRep")));

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	FString RepNotifyGraph;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetStringField(TEXT("rep_notify_graph"), RepNotifyGraph));
	UNTEST_EXPECT_STREQ(*RepNotifyGraph, TEXT("Custom_OnRep"));

	// Custom graph created, default OnRep_CustomVar was NOT created.
	UNTEST_EXPECT_TRUE(HasFunctionGraph(BP, TEXT("Custom_OnRep")));
	UNTEST_EXPECT_FALSE(HasFunctionGraph(BP, TEXT("OnRep_CustomVar")));

	const FBPVariableDescription* VarDesc = FindVarDesc(BP, TEXT("CustomVar"));
	UNTEST_ASSERT_PTR(VarDesc);
	UNTEST_EXPECT_STREQ(*VarDesc->RepNotifyFunc.ToString(), TEXT("Custom_OnRep"));

	CleanupBlueprintAsset(TestBPPath_Custom);
	co_return;
}

// ============================================================================
// Test 3: SetVariableProperties idempotency -- pointer identity preserved
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, AddVarRepNotify, Functional_SetVariablePropertiesIdempotent, UNTEST_TIMEOUTMS(60000))
{
	CleanupBlueprintAsset(TestBPPath_Idempotent);

	UBlueprint* BP = CreatePlainActorBlueprint(TestBPPath_Idempotent);
	UNTEST_ASSERT_PTR(BP);

	// First: create a non-replicated variable via AddVariable.
	{
		ClaireonBlueprintGraphTool_AddVariable AddVarTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), TestBPPath_Idempotent);
		Args->SetStringField(TEXT("variable_name"), TEXT("IdempVar"));
		Args->SetStringField(TEXT("variable_type"), TEXT("int"));
		IClaireonTool::FToolResult AddResult = AddVarTool.Execute(Args);
		UNTEST_ASSERT_FALSE(AddResult.bIsError);
	}

	// Second: flip to RepNotify via SetVariableProperties. Expect OnRep_IdempVar created.
	ClaireonBlueprintGraphTool_SetVariableProperties SetPropsTool;
	IClaireonTool::FToolResult Result1 = SetPropsTool.Execute(
		MakeSetVarPropsArgs(TestBPPath_Idempotent, TEXT("IdempVar"), TEXT("RepNotify")));
	UNTEST_ASSERT_FALSE(Result1.bIsError);
	UNTEST_ASSERT_TRUE(Result1.Data.IsValid());

	FString RepNotifyGraph1;
	UNTEST_ASSERT_TRUE(Result1.Data->TryGetStringField(TEXT("rep_notify_graph"), RepNotifyGraph1));
	UNTEST_EXPECT_STREQ(*RepNotifyGraph1, TEXT("OnRep_IdempVar"));

	const int32 FirstCount = BP->FunctionGraphs.Num();
	UEdGraph* FirstPtr = FindFunctionGraph(BP, TEXT("OnRep_IdempVar"));
	UNTEST_ASSERT_PTR(FirstPtr);

	// Third: re-invoke the same call; graph must be reused, not re-created.
	IClaireonTool::FToolResult Result2 = SetPropsTool.Execute(
		MakeSetVarPropsArgs(TestBPPath_Idempotent, TEXT("IdempVar"), TEXT("RepNotify")));
	UNTEST_ASSERT_FALSE(Result2.bIsError);
	UNTEST_ASSERT_TRUE(Result2.Data.IsValid());

	FString RepNotifyGraph2;
	UNTEST_ASSERT_TRUE(Result2.Data->TryGetStringField(TEXT("rep_notify_graph"), RepNotifyGraph2));
	UNTEST_EXPECT_STREQ(*RepNotifyGraph2, TEXT("OnRep_IdempVar"));

	// No new graph added; pointer identity preserved.
	UNTEST_EXPECT_TRUE(BP->FunctionGraphs.Num() == FirstCount);
	UEdGraph* SecondPtr = FindFunctionGraph(BP, TEXT("OnRep_IdempVar"));
	UNTEST_EXPECT_TRUE(SecondPtr == FirstPtr);

	CleanupBlueprintAsset(TestBPPath_Idempotent);
	co_return;
}

// ============================================================================
// Test 4: snake_case replication='rep_notify' alias
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, AddVarRepNotify, Functional_SnakeCaseReplicationAlias, UNTEST_TIMEOUTMS(60000))
{
	CleanupBlueprintAsset(TestBPPath_Snake);

	UBlueprint* BP = CreatePlainActorBlueprint(TestBPPath_Snake);
	UNTEST_ASSERT_PTR(BP);

	ClaireonBlueprintGraphTool_AddVariable AddVarTool;
	IClaireonTool::FToolResult Result = AddVarTool.Execute(
		MakeAddVarArgs(TestBPPath_Snake, TEXT("SnakeVar"), TEXT("int"), TEXT("rep_notify")));

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	FString RepNotifyGraph;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetStringField(TEXT("rep_notify_graph"), RepNotifyGraph));
	UNTEST_EXPECT_STREQ(*RepNotifyGraph, TEXT("OnRep_SnakeVar"));

	UNTEST_EXPECT_TRUE(HasFunctionGraph(BP, TEXT("OnRep_SnakeVar")));

	const FBPVariableDescription* VarDesc = FindVarDesc(BP, TEXT("SnakeVar"));
	UNTEST_ASSERT_PTR(VarDesc);
	const uint64 RepFlags = VarDesc->PropertyFlags & (CPF_Net | CPF_RepNotify);
	UNTEST_EXPECT_TRUE(RepFlags == (CPF_Net | CPF_RepNotify));

	CleanupBlueprintAsset(TestBPPath_Snake);
	co_return;
}

// ============================================================================
// Test 5: non-RepNotify modes must NOT surface rep_notify_graph
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, AddVarRepNotify, Functional_ReplicatedModeDoesNotSurfaceRepNotifyGraph, UNTEST_TIMEOUTMS(60000))
{
	CleanupBlueprintAsset(TestBPPath_NoGraph);

	UBlueprint* BP = CreatePlainActorBlueprint(TestBPPath_NoGraph);
	UNTEST_ASSERT_PTR(BP);

	ClaireonBlueprintGraphTool_AddVariable AddVarTool;
	IClaireonTool::FToolResult Result = AddVarTool.Execute(
		MakeAddVarArgs(TestBPPath_NoGraph, TEXT("PlainVar"), TEXT("int"), TEXT("Replicated")));

	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	// rep_notify_graph field must NOT be present for non-RepNotify replication.
	UNTEST_EXPECT_FALSE(Result.Data->HasField(TEXT("rep_notify_graph")));

	// And no OnRep_PlainVar graph created.
	UNTEST_EXPECT_FALSE(HasFunctionGraph(BP, TEXT("OnRep_PlainVar")));

	CleanupBlueprintAsset(TestBPPath_NoGraph);
	co_return;
}

#endif // WITH_UNTESTED
