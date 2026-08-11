// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for apply_spec operation across all 8 session-based tools.
// Each test generates an asset from a declarative spec, then inspects it
// to verify the entities were created correctly.

#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonTestDataAssertions.h"
#include "ClaireonTestSchemaDiscovery.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "Tools/IClaireonTool.h"

// Edit tools (apply_spec targets -- decomposed per-system tools)
#include "Tools/ClaireonBehaviorTreeTool_ApplySpec.h"
#include "Tools/ClaireonBlueprintGraphTool_ApplySpec.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_AddVariable.h"
#include "Tools/ClaireonStateTreeTool_ApplySpec.h"
#include "Tools/ClaireonStateTreeTool_Create.h"
#include "Tools/ClaireonBlackboardTool_ApplySpec.h"
#include "Tools/ClaireonEQSTool_ApplySpec.h"
#include "Tools/ClaireonNiagaraTool_ApplySpec.h"
#include "Tools/ClaireonPCGGraphTool_ApplySpec.h"
#include "Tools/ClaireonWidgetBPTool_ApplySpec.h"
#include "Tools/ClaireonWidgetBPTool_Create.h"

// Inspect tools (verification)
#include "Tools/ClaireonTool_BehaviorTreeInspect.h"
#include "Tools/ClaireonTool_GetBlueprintGraph.h"
#include "Tools/ClaireonTool_StateTreeInspect.h"
#include "Tools/ClaireonTool_BehaviorTreeInspectBlackboard.h"
#include "Tools/ClaireonTool_EQSInspect.h"
#include "Tools/ClaireonTool_NiagaraInspect.h"
#include "Tools/ClaireonTool_PCGGraphInspect.h"
#include "Tools/ClaireonTool_GetWidgetBPTree.h"

// UE includes
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ObjectTools.h"
#include "PCGGraph.h"
#include "PCGNode.h"
#include "UObject/Package.h"

#include "ClaireonTestAssetDeletion.h"
// ---------------------------------------------------------------------------
// Test asset paths
//
// NOTHING here may point at real content that a test then mutates. apply_spec
// writes through to the asset, so pointing a test at shipping content dirties
// tracked .uasset files on every run AND makes the suite order-dependent: a
// later test inspects whatever an earlier one left behind. That is exactly how
// EQS.InspectStrafe came to pass in isolation and fail in the full run.
//
// The rule: a Source* path is READ-ONLY and is duplicated into /Game/__MCPTests;
// only the duplicate is ever edited, and it is deleted on the way out. The
// Niagara pair below established this pattern; BT/BB/EQS/ST now follow it.
// ---------------------------------------------------------------------------
static const TCHAR* ApplySpecSourceBTPath      = TEXT("/Game/BP/AI/BT/BT_CombatAttacking_Default");
static const TCHAR* ApplySpecTestBTPath        = TEXT("/Game/__MCPTests/BT_ApplySpecTest");
static const TCHAR* ApplySpecSourceBBPath      = TEXT("/Game/BP/AI/BT/BB_AI_Default");
static const TCHAR* ApplySpecTestBBPath        = TEXT("/Game/__MCPTests/BB_ApplySpecTest");
static const TCHAR* ApplySpecSourceEQSPath     = TEXT("/Game/BP/AI/EQS/EQS_CombatWaiting_Strafe");
static const TCHAR* ApplySpecTestEQSPath       = TEXT("/Game/__MCPTests/EQS_ApplySpecTest");
// No Source pair for StateTree: there is no StateTree asset in the repo to copy
// (/Game/BP/AI/ST/ is empty), so the fixture is built by statetree_create.
static const TCHAR* ApplySpecTestSTPath        = TEXT("/Game/__MCPTests/ST_ApplySpecTest");
static const TCHAR* ApplySpecTestBPPath        = TEXT("/Game/__MCPTests/BP_ApplySpecTest");
static const TCHAR* ApplySpecTestBPParityPath_A = TEXT("/Game/__MCPTests/BP_ApplySpecTest_ParityA");
static const TCHAR* ApplySpecTestBPParityPath_B = TEXT("/Game/__MCPTests/BP_ApplySpecTest_ParityB");
static const TCHAR* ApplySpecSourceNiagaraPath = TEXT("/Game/Art_Lib/VOL/NS_LocalVolumeFog");
static const TCHAR* ApplySpecTestNiagaraPath   = TEXT("/Game/__MCPTests/NS_ApplySpecTest");
static const TCHAR* ApplySpecTestWidgetPath    = TEXT("/Game/__MCPTests/WBP_ApplySpecTest");
static const TCHAR* ApplySpecTestPCGPath       = TEXT("/Game/__MCPTests/PCG_ApplySpecTest");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace ClaireonApplySpecTests_Private
{

// Decomposed *_apply_spec tools take flat args (no operation/params wrapper).
TSharedPtr<FJsonObject> MakeApplySpecArgsFlat(const TCHAR* AssetPath, const TSharedPtr<FJsonObject>& Spec)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetObjectField(TEXT("spec"), Spec);
	return Args;
}

TSharedPtr<FJsonObject> MakeInspectArgs(const TCHAR* AssetPath)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	return Args;
}

TSharedPtr<FJsonValue> MakeStr(const FString& Val)
{
	return MakeShared<FJsonValueString>(Val);
}

TSharedPtr<FJsonValue> MakeObj(const TSharedPtr<FJsonObject>& Obj)
{
	return MakeShared<FJsonValueObject>(Obj);
}

TSharedPtr<FJsonValue> MakeNull()
{
	return MakeShared<FJsonValueNull>();
}

TSharedPtr<FJsonObject> MakeBTNode(const FString& Id, const FString& Type,
	const TSharedPtr<FJsonValue>& Parent, const TArray<FString>& Children = {})
{
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("id"), Id);
	Node->SetStringField(TEXT("type"), Type);
	Node->SetField(TEXT("parent"), Parent);
	if (Children.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> ChildArray;
		for (const FString& C : Children)
		{
			ChildArray.Add(MakeStr(C));
		}
		Node->SetArrayField(TEXT("children"), ChildArray);
	}
	return Node;
}

// Verify apply_spec result: not error, has id_mappings and entries, all entries "ok"
bool VerifyApplySpecResult(const IClaireonTool::FToolResult& Result, int32 ExpectedEntries)
{
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[ApplySpec] Error: %s"), *Result.ErrorMessage);
		return false;
	}
	if (!Result.Data.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("[ApplySpec] Result has no Data"));
		return false;
	}

	const TSharedPtr<FJsonObject>* IdMappings = nullptr;
	if (!Result.Data->TryGetObjectField(TEXT("id_mappings"), IdMappings) || !IdMappings || !(*IdMappings).IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("[ApplySpec] Missing 'id_mappings'"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!Result.Data->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
	{
		UE_LOG(LogTemp, Error, TEXT("[ApplySpec] Missing 'entries' array"));
		return false;
	}

	if (Entries->Num() < ExpectedEntries)
	{
		UE_LOG(LogTemp, Error, TEXT("[ApplySpec] Expected %d entries, got %d"), ExpectedEntries, Entries->Num());
		return false;
	}

	int32 OkCount = 0;
	for (int32 i = 0; i < Entries->Num(); ++i)
	{
		const TSharedPtr<FJsonObject>& Entry = (*Entries)[i]->AsObject();
		if (!Entry.IsValid()) continue;

		FString Status, SpecId;
		Entry->TryGetStringField(TEXT("status"), Status);
		Entry->TryGetStringField(TEXT("spec_id"), SpecId);
		if (Status == TEXT("ok"))
		{
			OkCount++;
			FString ActualId;
			if (!Entry->TryGetStringField(TEXT("actual_id"), ActualId) || ActualId.IsEmpty())
			{
				UE_LOG(LogTemp, Warning, TEXT("[ApplySpec] Entry '%s' ok but missing actual_id"), *SpecId);
			}
		}
		else
		{
			FString Error;
			Entry->TryGetStringField(TEXT("error"), Error);
			UE_LOG(LogTemp, Error, TEXT("[ApplySpec] Entry '%s' status=%s error=%s"), *SpecId, *Status, *Error);
		}
	}

	if (OkCount < ExpectedEntries)
	{
		UE_LOG(LogTemp, Error, TEXT("[ApplySpec] Only %d/%d entries ok"), OkCount, ExpectedEntries);
		return false;
	}
	return true;
}

// Delete a fixture this test created.
//
// DeleteObjectsUnchecked, NOT ForceDeleteObjects. ForceDeleteObjects runs
// RecursiveRetrieveReferencers, which walks EVERY live UObject with a reference-finding
// archive (FReferencerFinderArchive on the default path, FFindReferencersArchive under
// Editor.UseLegacyGetReferencersForDeletion). Both of those call UObject::Serialize on
// each candidate, and serializing a resident UNiagaraEmitter that way crashes with an
// access violation inside its nested struct arrays -- reproducibly, whenever any earlier
// test has pulled a Niagara asset into memory. That crash killed whole suite runs, not
// just the deleting test, and flipping the CVar only moves it between the two archives.
//
// The reference check buys nothing here: the fixture was created moments ago by this
// test and cannot have acquired outside referencers. DeleteObjectsUnchecked skips the
// scan (bPerformReferenceCheck=false) and still deletes the package from disk via
// CleanupAfterSuccessfulDelete, so cleanup semantics are unchanged.
void CleanupTestAsset(const FString& AssetPath)
{
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (IsValid(Asset))
	{
		TArray<UObject*> AssetsToDelete;
		AssetsToDelete.Add(Asset);
		ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
	}
}

// Serialize a tool result's structured Data to JSON so a test can assert that
// something it created shows up in the tool's output.
//
// Do NOT use GetContentAsString() for this. Every inspect tool's text content is
// just a summary line -- behaviortree_inspect emits "<AssetName>: <N> nodes",
// blackboard_inspect "<AssetName>: <N> keys (...)", eqs_inspect
// "<AssetName>: <N> generator(s), <N> test(s)". A node class or key name can
// never appear in those strings, so asserting Contains("Selector") against the
// content is unpassable by construction no matter what the tool did. The node
// and key detail lives in Data (Data.structure, Data.own_keys, Data.generators,
// ...), which is what this serializes.
FString ApplySpecTest_DataToString(const IClaireonTool::FToolResult& Result)
{
	if (!Result.Data.IsValid())
	{
		return FString();
	}
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Result.Data.ToSharedRef(), Writer);
	return Out;
}

// Duplicate SourcePath -> DestPath, deleting any pre-existing DestPath first.
//
// The leading delete is not paranoia: /Game/__MCPTests is NOT gitignored and
// survives between runs, so a run that dies mid-test leaves a mutated fixture
// that the next run would otherwise inherit and assert against.
// Build an EMPTY PCG graph asset at DestPath, deleting anything already there.
//
// Nothing in the repo ever created /Game/__MCPTests/PCG_ApplySpecTest, so
// ApplySpec_PCG was permanently skipped: apply_spec failed with "Failed to open",
// the test logged a warning and co_returned, and Untest scored that as a PASS.
// There is no pcg_create tool to build the fixture with and no reason to commit a
// tracked PCG graph for a test to mutate, so synthesize it here.
//
// NewObject<UPCGGraph> is exactly what the editor's own factory does
// (UPCGGraphFactory::FactoryCreateNew is a one-line NewObject), so this is a
// faithful empty graph, not an approximation.
//
// The package is real rather than in-memory-only on purpose: apply_spec's
// SaveAsset() calls UEditorLoadingAndSavingUtils::SavePackages unconditionally,
// so an unmountable /Memory/ package would only produce a save failure. The
// caller must therefore delete it on the way out, exactly like the duplicated
// BT/BB/EQS/Niagara fixtures above.
bool EnsureFreshPCGGraph(const TCHAR* DestPath)
{
	CleanupTestAsset(DestPath);

	UPackage* Package = CreatePackage(DestPath);
	if (!IsValid(Package))
	{
		return false;
	}

	const FString AssetName = FPackageName::GetShortName(FString(DestPath));
	UPCGGraph* Graph = NewObject<UPCGGraph>(
		Package,
		UPCGGraph::StaticClass(),
		FName(*AssetName),
		RF_Public | RF_Standalone | RF_Transactional);
	if (!IsValid(Graph))
	{
		return false;
	}

	FAssetRegistryModule::AssetCreated(Graph);
	Package->MarkPackageDirty();
	return true;
}

bool EnsureFreshDuplicate(const TCHAR* SourcePath, const TCHAR* DestPath)
{
	CleanupTestAsset(DestPath);
	return UEditorAssetLibrary::DuplicateAsset(SourcePath, DestPath) != nullptr;
}

} // namespace ClaireonApplySpecTests_Private
using namespace ClaireonApplySpecTests_Private;

// ============================================================================
// BehaviorTree -- apply_spec creates nodes in an existing BT
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_BehaviorTree, CreateNodesFromSpec, UNTEST_TIMEOUTMS(30000))
{
	UNTEST_ASSERT_TRUE(EnsureFreshDuplicate(ApplySpecSourceBTPath, ApplySpecTestBTPath));
	ON_SCOPE_EXIT { CleanupTestAsset(ApplySpecTestBTPath); };

	ClaireonBehaviorTreeTool_ApplySpec Tool;

	// Spec: Selector with two Wait task children
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(MakeObj(MakeBTNode(TEXT("sel1"), TEXT("BTComposite_Selector"),
		MakeNull(), {TEXT("wait1"), TEXT("wait2")})));
	Nodes.Add(MakeObj(MakeBTNode(TEXT("wait1"), TEXT("BTTask_Wait"), MakeStr(TEXT("sel1")))));
	Nodes.Add(MakeObj(MakeBTNode(TEXT("wait2"), TEXT("BTTask_Wait"), MakeStr(TEXT("sel1")))));
	Spec->SetArrayField(TEXT("nodes"), Nodes);

	auto Result = Tool.Execute(MakeApplySpecArgsFlat(ApplySpecTestBTPath, Spec));
	UNTEST_ASSERT_TRUE(VerifyApplySpecResult(Result, 3));

	// Verify id_mappings has all 3 spec IDs
	const TSharedPtr<FJsonObject>* Mappings = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("id_mappings"), Mappings));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("sel1")));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("wait1")));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("wait2")));

	// Verify via inspect: the BT contains Selector and Wait nodes
	ClaireonTool_BehaviorTreeInspect InspectTool;
	auto InspectResult = InspectTool.Execute(MakeInspectArgs(ApplySpecTestBTPath));
	UNTEST_ASSERT_FALSE(InspectResult.bIsError);
	FString InspectText = ApplySpecTest_DataToString(InspectResult);
	UNTEST_EXPECT_TRUE(InspectText.Contains(TEXT("Selector")));
	UNTEST_EXPECT_TRUE(InspectText.Contains(TEXT("Wait")));

	co_return;
}

// ============================================================================
// Blueprint -- apply_spec creates nodes and variables in a new BP
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_Blueprint, CreateGraphFromSpec, UNTEST_TIMEOUTMS(30000))
{
	ClaireonBlueprintGraphTool_ApplySpec Tool;

	// Create a test Blueprint using the decomposed Create tool (flat-args schema).
	{
		ClaireonBlueprintGraphTool_Create CreateTool;

		TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
		CreateArgs->SetStringField(TEXT("asset_path"), ApplySpecTestBPPath);
		CreateArgs->SetStringField(TEXT("parent_class"), TEXT("Actor"));

		auto CreateResult = CreateTool.Execute(CreateArgs);
		UNTEST_ASSERT_FALSE(CreateResult.bIsError);
	}

	// Spec: PrintString node + a variable
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	Spec->SetStringField(TEXT("graph"), TEXT("EventGraph"));

	TArray<TSharedPtr<FJsonValue>> SpecNodes;
	{
		TSharedPtr<FJsonObject> PrintNode = MakeShared<FJsonObject>();
		PrintNode->SetStringField(TEXT("id"), TEXT("print1"));
		PrintNode->SetStringField(TEXT("type"), TEXT("K2Node_CallFunction"));
		PrintNode->SetStringField(TEXT("function"), TEXT("KismetSystemLibrary.PrintString"));
		SpecNodes.Add(MakeObj(PrintNode));
	}
	Spec->SetArrayField(TEXT("nodes"), SpecNodes);

	TArray<TSharedPtr<FJsonValue>> Variables;
	{
		TSharedPtr<FJsonObject> Var = MakeShared<FJsonObject>();
		Var->SetStringField(TEXT("id"), TEXT("var_health"));
		Var->SetStringField(TEXT("name"), TEXT("TestHealth"));
		Var->SetStringField(TEXT("type"), TEXT("float"));
		Var->SetStringField(TEXT("default_value"), TEXT("100.0"));
		Variables.Add(MakeObj(Var));
	}
	Spec->SetArrayField(TEXT("variables"), Variables);

	auto Result = Tool.Execute(MakeApplySpecArgsFlat(ApplySpecTestBPPath, Spec));
	UNTEST_ASSERT_TRUE(VerifyApplySpecResult(Result, 2));

	const TSharedPtr<FJsonObject>* Mappings = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("id_mappings"), Mappings));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("print1")));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("var_health")));

	// Verify via inspect
	ClaireonTool_GetBlueprintGraph InspectTool;
	auto InspectArgs = MakeShared<FJsonObject>();
	InspectArgs->SetStringField(TEXT("asset_path"), ApplySpecTestBPPath);
	InspectArgs->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	auto InspectResult = InspectTool.Execute(InspectArgs);
	UNTEST_ASSERT_FALSE(InspectResult.bIsError);
	// "Print String", not "PrintString": get_blueprint_graph reports node_title from
	// the K2 node's display title, which is spaced. The spec asks for the function
	// KismetSystemLibrary.PrintString, but nothing in the response echoes that
	// member name verbatim.
	UNTEST_EXPECT_TRUE(ApplySpecTest_DataToString(InspectResult).Contains(TEXT("Print String")));

	CleanupTestAsset(ApplySpecTestBPPath);
	co_return;
}

// ============================================================================
// Blackboard -- apply_spec creates keys
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_Blackboard, CreateKeysFromSpec, UNTEST_TIMEOUTMS(30000))
{
	UNTEST_ASSERT_TRUE(EnsureFreshDuplicate(ApplySpecSourceBBPath, ApplySpecTestBBPath));
	ON_SCOPE_EXIT { CleanupTestAsset(ApplySpecTestBBPath); };

	ClaireonBlackboardTool_ApplySpec Tool;

	// Spec: 3 keys of different types
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Keys;
	{
		TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
		K->SetStringField(TEXT("id"), TEXT("key_target"));
		K->SetStringField(TEXT("name"), TEXT("TestTarget"));
		K->SetStringField(TEXT("type"), TEXT("Object"));
		Keys.Add(MakeObj(K));
	}
	{
		TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
		K->SetStringField(TEXT("id"), TEXT("key_combat"));
		K->SetStringField(TEXT("name"), TEXT("TestInCombat"));
		K->SetStringField(TEXT("type"), TEXT("Bool"));
		Keys.Add(MakeObj(K));
	}
	{
		TSharedPtr<FJsonObject> K = MakeShared<FJsonObject>();
		K->SetStringField(TEXT("id"), TEXT("key_health"));
		K->SetStringField(TEXT("name"), TEXT("TestHealthThreshold"));
		K->SetStringField(TEXT("type"), TEXT("Float"));
		Keys.Add(MakeObj(K));
	}
	Spec->SetArrayField(TEXT("keys"), Keys);

	// Decomposed apply_spec tool accepts asset_path + spec directly on the top-level args.
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), ApplySpecTestBBPath);
	Args->SetObjectField(TEXT("spec"), Spec);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(VerifyApplySpecResult(Result, 3));

	const TSharedPtr<FJsonObject>* Mappings = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("id_mappings"), Mappings));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("key_target")));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("key_combat")));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("key_health")));

	// Verify via inspect
	ClaireonTool_BehaviorTreeInspectBlackboard InspectTool;
	auto InspectResult = InspectTool.Execute(MakeInspectArgs(ApplySpecTestBBPath));
	UNTEST_ASSERT_FALSE(InspectResult.bIsError);
	FString InspectText = ApplySpecTest_DataToString(InspectResult);
	UNTEST_EXPECT_TRUE(InspectText.Contains(TEXT("TestTarget")));
	UNTEST_EXPECT_TRUE(InspectText.Contains(TEXT("TestInCombat")));
	UNTEST_EXPECT_TRUE(InspectText.Contains(TEXT("TestHealthThreshold")));

	co_return;
}

// ============================================================================
// EQS -- apply_spec creates options with generators and tests
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_EQS, CreateOptionsFromSpec, UNTEST_TIMEOUTMS(30000))
{
	UNTEST_ASSERT_TRUE(EnsureFreshDuplicate(ApplySpecSourceEQSPath, ApplySpecTestEQSPath));
	ON_SCOPE_EXIT { CleanupTestAsset(ApplySpecTestEQSPath); };

	ClaireonEQSTool_ApplySpec Tool;

	// Spec: 1 option with SimpleGrid generator and Distance test
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Options;
	{
		TSharedPtr<FJsonObject> Opt = MakeShared<FJsonObject>();
		Opt->SetStringField(TEXT("id"), TEXT("opt1"));

		TSharedPtr<FJsonObject> Gen = MakeShared<FJsonObject>();
		Gen->SetStringField(TEXT("type"), TEXT("EnvQueryGenerator_SimpleGrid"));
		Opt->SetObjectField(TEXT("generator"), Gen);

		TArray<TSharedPtr<FJsonValue>> Tests;
		{
			TSharedPtr<FJsonObject> T = MakeShared<FJsonObject>();
			T->SetStringField(TEXT("id"), TEXT("test1"));
			T->SetStringField(TEXT("type"), TEXT("EnvQueryTest_Distance"));
			Tests.Add(MakeObj(T));
		}
		Opt->SetArrayField(TEXT("tests"), Tests);
		Options.Add(MakeObj(Opt));
	}
	Spec->SetArrayField(TEXT("options"), Options);

	// Decomposed apply_spec tool accepts asset_path + spec directly on the top-level args.
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), ApplySpecTestEQSPath);
	Args->SetObjectField(TEXT("spec"), Spec);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(VerifyApplySpecResult(Result, 2));

	const TSharedPtr<FJsonObject>* Mappings = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("id_mappings"), Mappings));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("opt1")));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("test1")));

	// Verify via inspect
	ClaireonTool_EQSInspect InspectTool;
	auto InspectResult = InspectTool.Execute(MakeInspectArgs(ApplySpecTestEQSPath));
	UNTEST_ASSERT_FALSE(InspectResult.bIsError);
	UNTEST_EXPECT_TRUE(ApplySpecTest_DataToString(InspectResult).Contains(TEXT("SimpleGrid")));

	co_return;
}

// ============================================================================
// StateTree -- apply_spec creates states
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_StateTree, CreateStatesFromSpec, UNTEST_TIMEOUTMS(30000))
{
	// This test used to target /Game/BP/AI/ST/ST_TestDummy, which does not exist
	// anywhere in the repo -- /Game/BP/AI/ST/ is empty and nothing is tracked
	// there. apply_spec therefore failed on a missing asset, which is why this
	// test's failure looked different from its siblings'. Build the fixture with
	// statetree_create instead of adding a tracked asset for a test to mutate.
	CleanupTestAsset(ApplySpecTestSTPath);
	{
		// UStateTreeSchema itself is the abstract base and the factory rejects it, so
		// the fixture needs a concrete subclass. Claireon ships none of its own, so
		// discover one from the running project (ClaireonTestSchemaDiscovery). Soft
		// dependency: if no concrete schema is loaded, skip rather than fail.
		const FString SchemaClassPath = ClaireonTestSchemaDiscovery::FindConcreteStateTreeSchemaClassPath();
		ClaireonStateTreeTool_Create CreateTool;
		TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
		CreateArgs->SetStringField(TEXT("asset_path"), ApplySpecTestSTPath);
		CreateArgs->SetStringField(TEXT("schema_class_path"), SchemaClassPath);
		const auto CreateResult = CreateTool.Execute(CreateArgs);
		if (CreateResult.bIsError)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[ApplySpec_StateTree] Could not create a StateTree fixture (%s); skipping."),
				*CreateResult.ErrorMessage);
			co_return;
		}
	}
	ON_SCOPE_EXIT { CleanupTestAsset(ApplySpecTestSTPath); };

	ClaireonStateTreeTool_ApplySpec Tool;

	// Spec: 2 root-level states
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> States;
	{
		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("id"), TEXT("state_idle"));
		S->SetStringField(TEXT("name"), TEXT("TestIdle"));
		S->SetStringField(TEXT("type"), TEXT("State"));
		S->SetField(TEXT("parent"), MakeNull());
		States.Add(MakeObj(S));
	}
	{
		TSharedPtr<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("id"), TEXT("state_combat"));
		S->SetStringField(TEXT("name"), TEXT("TestCombat"));
		S->SetStringField(TEXT("type"), TEXT("State"));
		S->SetField(TEXT("parent"), MakeNull());
		States.Add(MakeObj(S));
	}
	Spec->SetArrayField(TEXT("states"), States);

	auto Result = Tool.Execute(MakeApplySpecArgsFlat(ApplySpecTestSTPath, Spec));
	UNTEST_ASSERT_TRUE(VerifyApplySpecResult(Result, 2));

	const TSharedPtr<FJsonObject>* Mappings = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("id_mappings"), Mappings));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("state_idle")));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("state_combat")));

	// Verify via inspect
	ClaireonTool_StateTreeInspect InspectTool;
	auto InspectResult = InspectTool.Execute(MakeInspectArgs(ApplySpecTestSTPath));
	UNTEST_ASSERT_FALSE(InspectResult.bIsError);
	FString InspectText = ApplySpecTest_DataToString(InspectResult);
	UNTEST_EXPECT_TRUE(InspectText.Contains(TEXT("TestIdle")));
	UNTEST_EXPECT_TRUE(InspectText.Contains(TEXT("TestCombat")));

	co_return;
}

// ============================================================================
// Niagara -- apply_spec adds parameters to an existing system
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_Niagara, CreateParametersFromSpec, UNTEST_TIMEOUTMS(30000))
{
	// apply_spec saves the target package; work on a throwaway duplicate.
	if (UEditorAssetLibrary::DoesAssetExist(ApplySpecTestNiagaraPath))
	{
		ClaireonTestAssetDeletion::DeleteAssetForTest(ApplySpecTestNiagaraPath);
	}
	UNTEST_ASSERT_TRUE(UEditorAssetLibrary::DuplicateAsset(ApplySpecSourceNiagaraPath, ApplySpecTestNiagaraPath) != nullptr);
	ON_SCOPE_EXIT
	{
		ClaireonTestAssetDeletion::DeleteAssetForTest(ApplySpecTestNiagaraPath);
	};

	ClaireonNiagaraTool_ApplySpec Tool;

	// Spec: add 2 system-level parameters
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Parameters;
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("id"), TEXT("param_intensity"));
		P->SetStringField(TEXT("name"), TEXT("TestIntensity"));
		P->SetStringField(TEXT("type"), TEXT("float"));
		P->SetStringField(TEXT("value"), TEXT("1.5"));
		Parameters.Add(MakeObj(P));
	}
	{
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("id"), TEXT("param_color"));
		P->SetStringField(TEXT("name"), TEXT("TestColor"));
		P->SetStringField(TEXT("type"), TEXT("LinearColor"));
		P->SetStringField(TEXT("value"), TEXT("(R=1.0,G=0.5,B=0.0,A=1.0)"));
		Parameters.Add(MakeObj(P));
	}
	Spec->SetArrayField(TEXT("parameters"), Parameters);
	Spec->SetArrayField(TEXT("emitters"), TArray<TSharedPtr<FJsonValue>>());

	// Decomposed apply_spec tool accepts asset_path + spec directly on the top-level args.
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), ApplySpecTestNiagaraPath);
	Args->SetObjectField(TEXT("spec"), Spec);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(VerifyApplySpecResult(Result, 2));

	const TSharedPtr<FJsonObject>* Mappings = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("id_mappings"), Mappings));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("param_intensity")));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("param_color")));

	// Verify via inspect. The tool returns user parameters in the structured "parameters"
	// field of Data (the content string is only a one-line summary), so assert against that.
	ClaireonTool_NiagaraInspect InspectTool;
	auto InspectResult = InspectTool.Execute(MakeInspectArgs(ApplySpecTestNiagaraPath));
	UNTEST_ASSERT_FALSE(InspectResult.bIsError);
	UNTEST_ASSERT_PTR(InspectResult.Data.Get());
	FString ParamsText;
	InspectResult.Data->TryGetStringField(TEXT("parameters"), ParamsText);
	UNTEST_EXPECT_TRUE(ParamsText.Contains(TEXT("TestIntensity")));

	co_return;
}

// ============================================================================
// PCG -- apply_spec creates nodes and connections
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_PCG, CreateNodesFromSpec, UNTEST_TIMEOUTMS(30000))
{
	// The fixture is built here rather than assumed to exist: nothing in the repo
	// ever created /Game/__MCPTests/PCG_ApplySpecTest, so this test spent its whole
	// life on the "Test asset not found, skipping" branch -- which Untest scores as
	// a PASS. See EnsureFreshPCGGraph for why the package is real, not in-memory.
	UNTEST_ASSERT_TRUE(EnsureFreshPCGGraph(ApplySpecTestPCGPath));
	ON_SCOPE_EXIT { CleanupTestAsset(ApplySpecTestPCGPath); };

	ClaireonPCGGraphTool_ApplySpec Tool;

	// Spec: 2 PCG nodes with a connection
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> SpecNodes;
	{
		TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
		N->SetStringField(TEXT("id"), TEXT("surf_sampler"));
		N->SetStringField(TEXT("type"), TEXT("PCGSurfaceSamplerSettings"));
		SpecNodes.Add(MakeObj(N));
	}
	{
		TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
		N->SetStringField(TEXT("id"), TEXT("static_mesh"));
		N->SetStringField(TEXT("type"), TEXT("PCGStaticMeshSpawnerSettings"));
		SpecNodes.Add(MakeObj(N));
	}
	Spec->SetArrayField(TEXT("nodes"), SpecNodes);

	TArray<TSharedPtr<FJsonValue>> Connections;
	{
		TSharedPtr<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("source_node"), TEXT("surf_sampler"));
		C->SetStringField(TEXT("source_pin"), TEXT("Out"));
		C->SetStringField(TEXT("target_node"), TEXT("static_mesh"));
		C->SetStringField(TEXT("target_pin"), TEXT("In"));
		Connections.Add(MakeObj(C));
	}
	Spec->SetArrayField(TEXT("connections"), Connections);

	// Decomposed apply_spec tool accepts asset_path + spec directly on the top-level args.
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), ApplySpecTestPCGPath);
	Args->SetObjectField(TEXT("spec"), Spec);
	auto Result = Tool.Execute(Args);

	// No "asset might not exist" branch any more: the fixture was created above, so
	// a "Failed to open" here is a real failure, not an environment we tolerate.
	UNTEST_ASSERT_TRUE(VerifyApplySpecResult(Result, 2));

	const TSharedPtr<FJsonObject>* Mappings = nullptr;
	UNTEST_CLAIREON_DATA_OBJECT(Result, "id_mappings", Mappings);
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("surf_sampler")));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("static_mesh")));

	// Verify against the graph the tool actually wrote. The fixture started EMPTY,
	// so the node count is exact, and the connection from Pass 2 must be live on
	// both ends. Previously the only post-condition was "inspect returned no
	// error", which an inspect of an empty graph satisfies just as well.
	FString LoadError;
	UPCGGraph* Graph = ClaireonPCGGraphHelpers::LoadPCGGraphAsset(ApplySpecTestPCGPath, LoadError);
	UNTEST_ASSERT_PTR(Graph);

	const TArray<UPCGNode*>& GraphNodes = Graph->GetNodes();
	UNTEST_ASSERT_EQ(GraphNodes.Num(), 2);

	int32 ConnectedOutPins = 0;
	int32 ConnectedInPins = 0;
	for (UPCGNode* Node : GraphNodes)
	{
		if (!IsValid(Node))
		{
			continue;
		}
		if (Node->IsOutputPinConnected(TEXT("Out")))
		{
			++ConnectedOutPins;
		}
		if (Node->IsInputPinConnected(TEXT("In")))
		{
			++ConnectedInPins;
		}
	}
	UNTEST_EXPECT_EQ(ConnectedOutPins, 1);
	UNTEST_EXPECT_EQ(ConnectedInPins, 1);

	ClaireonTool_PCGGraphInspect InspectTool;
	auto InspectResult = InspectTool.Execute(MakeInspectArgs(ApplySpecTestPCGPath));
	UNTEST_ASSERT_FALSE(InspectResult.bIsError);

	co_return;
}

// ============================================================================
// WidgetBP -- apply_spec creates widget hierarchy in a new Widget Blueprint
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_WidgetBP, CreateWidgetsFromSpec, UNTEST_TIMEOUTMS(30000))
{
	// Create a test Widget Blueprint using the decomposed Create tool (flat args).
	{
		ClaireonWidgetBPTool_Create CreateTool;

		TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
		CreateArgs->SetStringField(TEXT("asset_path"), ApplySpecTestWidgetPath);

		auto CreateResult = CreateTool.Execute(CreateArgs);
		if (CreateResult.bIsError)
		{
			UE_LOG(LogTemp, Warning, TEXT("[ApplySpec_WidgetBP] Could not create test widget: %s"),
				*CreateResult.GetContentAsString());
			co_return;
		}
	}

	// Spec: CanvasPanel with TextBlock and Button children
	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Widgets;
	{
		TSharedPtr<FJsonObject> W = MakeShared<FJsonObject>();
		W->SetStringField(TEXT("id"), TEXT("canvas"));
		W->SetStringField(TEXT("type"), TEXT("CanvasPanel"));
		W->SetField(TEXT("parent"), MakeNull());
		W->SetArrayField(TEXT("children"), {MakeStr(TEXT("title_text")), MakeStr(TEXT("action_btn"))});
		Widgets.Add(MakeObj(W));
	}
	{
		TSharedPtr<FJsonObject> W = MakeShared<FJsonObject>();
		W->SetStringField(TEXT("id"), TEXT("title_text"));
		W->SetStringField(TEXT("type"), TEXT("TextBlock"));
		W->SetStringField(TEXT("parent"), TEXT("canvas"));
		Widgets.Add(MakeObj(W));
	}
	{
		TSharedPtr<FJsonObject> W = MakeShared<FJsonObject>();
		W->SetStringField(TEXT("id"), TEXT("action_btn"));
		W->SetStringField(TEXT("type"), TEXT("Button"));
		W->SetStringField(TEXT("parent"), TEXT("canvas"));
		Widgets.Add(MakeObj(W));
	}
	Spec->SetArrayField(TEXT("widgets"), Widgets);

	// Invoke the decomposed apply_spec tool with flat args.
	ClaireonWidgetBPTool_ApplySpec Tool;
	auto Result = Tool.Execute(MakeApplySpecArgsFlat(ApplySpecTestWidgetPath, Spec));
	UNTEST_ASSERT_TRUE(VerifyApplySpecResult(Result, 3));

	const TSharedPtr<FJsonObject>* Mappings = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("id_mappings"), Mappings));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("canvas")));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("title_text")));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("action_btn")));

	// Verify via inspect
	ClaireonTool_GetWidgetBPTree InspectTool;
	auto InspectResult = InspectTool.Execute(MakeInspectArgs(ApplySpecTestWidgetPath));
	UNTEST_ASSERT_FALSE(InspectResult.bIsError);
	FString InspectText = ApplySpecTest_DataToString(InspectResult);
	UNTEST_EXPECT_TRUE(InspectText.Contains(TEXT("CanvasPanel")));
	UNTEST_EXPECT_TRUE(InspectText.Contains(TEXT("TextBlock")));
	UNTEST_EXPECT_TRUE(InspectText.Contains(TEXT("Button")));

	CleanupTestAsset(ApplySpecTestWidgetPath);
	co_return;
}

// ============================================================================
// Validation -- apply_spec rejects invalid specs
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_Validation, RejectsEmptySpec, UNTEST_TIMEOUTMS(5000))
{
	ClaireonBehaviorTreeTool_ApplySpec Tool;

	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(MakeApplySpecArgsFlat(ApplySpecTestBTPath, Spec));
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("validation")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ApplySpec_Validation, RejectsMissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonBehaviorTreeTool_ApplySpec Tool;

	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetObjectField(TEXT("spec"), Spec);

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("asset_path")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ApplySpec_Validation, RejectsMissingNodeId, UNTEST_TIMEOUTMS(5000))
{
	ClaireonBehaviorTreeTool_ApplySpec Tool;

	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Nodes;
	{
		TSharedPtr<FJsonObject> BadNode = MakeShared<FJsonObject>();
		BadNode->SetStringField(TEXT("type"), TEXT("BTComposite_Selector"));
		BadNode->SetField(TEXT("parent"), MakeNull());
		Nodes.Add(MakeObj(BadNode));
	}
	Spec->SetArrayField(TEXT("nodes"), Nodes);

	auto Result = Tool.Execute(MakeApplySpecArgsFlat(ApplySpecTestBTPath, Spec));
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("id")));

	co_return;
}

// ============================================================================
// Blueprint parity -- the standalone-tool sequence and apply_spec must agree
// on the helper outputs (parent_class via CreateBlueprint, variables via
// CreateVariableFromSpec).
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpecBlueprintParity, BlueprintParity, UNTEST_TIMEOUTMS(60000))
{
	// Clean any leftovers from prior runs.
	CleanupTestAsset(ApplySpecTestBPParityPath_A);
	CleanupTestAsset(ApplySpecTestBPParityPath_B);

	// ----------------------------------------------------------------------
	// PATH A: standalone tools (Create -> AddVariable). Both route through the
	// shared helpers (ClaireonBlueprintHelpers::CreateBlueprint and
	// ClaireonBlueprintHelpers::CreateVariableFromSpec) per stages 003/004.
	// ----------------------------------------------------------------------
	FString SessionIdA;
	{
		ClaireonBlueprintGraphTool_Create CreateTool;
		TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
		CreateArgs->SetStringField(TEXT("asset_path"), ApplySpecTestBPParityPath_A);
		CreateArgs->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		IClaireonTool::FToolResult CreateResultA = CreateTool.Execute(CreateArgs);
		UNTEST_ASSERT_FALSE(CreateResultA.bIsError);
		UNTEST_ASSERT_TRUE(CreateResultA.Data.IsValid());
		UNTEST_ASSERT_TRUE(CreateResultA.Data->TryGetStringField(TEXT("session_id"), SessionIdA));
		UNTEST_ASSERT_FALSE(SessionIdA.IsEmpty());

		ClaireonBlueprintGraphTool_AddVariable AddVarTool;
		TSharedPtr<FJsonObject> AddVarArgs = MakeShared<FJsonObject>();
		AddVarArgs->SetStringField(TEXT("session_id"), SessionIdA);
		AddVarArgs->SetStringField(TEXT("variable_name"), TEXT("TestHealth"));
		AddVarArgs->SetStringField(TEXT("variable_type"), TEXT("float"));
		AddVarArgs->SetStringField(TEXT("category"), TEXT("Combat"));
		AddVarArgs->SetStringField(TEXT("tooltip"), TEXT("Health for parity test"));
		AddVarArgs->SetBoolField(TEXT("instance_editable"), true);
		IClaireonTool::FToolResult AddVarResultA = AddVarTool.Execute(AddVarArgs);
		UNTEST_ASSERT_FALSE(AddVarResultA.bIsError);
	}

	// ----------------------------------------------------------------------
	// PATH B: apply_spec single-call (parent_class + same variable). Goes
	// through OpenOrCreateAsset -> CreateBlueprint and
	// ApplyPass1_CreateEntities -> CreateVariableFromSpec.
	// ----------------------------------------------------------------------
	{
		ClaireonBlueprintGraphTool_ApplySpec ApplySpecTool;
		TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
		Spec->SetStringField(TEXT("parent_class"), TEXT("Actor"));

		TArray<TSharedPtr<FJsonValue>> Variables;
		{
			TSharedPtr<FJsonObject> Var = MakeShared<FJsonObject>();
			Var->SetStringField(TEXT("id"), TEXT("var_health"));
			Var->SetStringField(TEXT("name"), TEXT("TestHealth"));
			Var->SetStringField(TEXT("type"), TEXT("float"));
			Var->SetStringField(TEXT("category"), TEXT("Combat"));
			Var->SetStringField(TEXT("tooltip"), TEXT("Health for parity test"));
			Var->SetBoolField(TEXT("instance_editable"), true);
			Variables.Add(MakeObj(Var));
		}
		Spec->SetArrayField(TEXT("variables"), Variables);

		IClaireonTool::FToolResult ResultB = ApplySpecTool.Execute(
			MakeApplySpecArgsFlat(ApplySpecTestBPParityPath_B, Spec));
		UNTEST_ASSERT_TRUE(VerifyApplySpecResult(ResultB, 1));
	}

	// ----------------------------------------------------------------------
	// PARITY ASSERTIONS
	// ----------------------------------------------------------------------
	UBlueprint* BP_A = LoadObject<UBlueprint>(nullptr, ApplySpecTestBPParityPath_A);
	UBlueprint* BP_B = LoadObject<UBlueprint>(nullptr, ApplySpecTestBPParityPath_B);
	UNTEST_ASSERT_TRUE(BP_A != nullptr);
	UNTEST_ASSERT_TRUE(BP_B != nullptr);

	// (1) Same ParentClass. Both paths resolve "Actor" to AActor via
	// ClaireonNameResolver::ResolveClassName and pass it to CreateBlueprint.
	UNTEST_EXPECT_EQ(BP_A->ParentClass, BP_B->ParentClass);

	// (2) Same variable count.
	UNTEST_ASSERT_EQ(BP_A->NewVariables.Num(), BP_B->NewVariables.Num());

	// (3) Per-variable parity: same name, type, flags, and category.
	for (int32 i = 0; i < BP_A->NewVariables.Num(); ++i)
	{
		const FBPVariableDescription& VarA = BP_A->NewVariables[i];
		const FName VarAName = VarA.VarName;
		const FBPVariableDescription* VarB = nullptr;
		for (const FBPVariableDescription& V : BP_B->NewVariables)
		{
			if (V.VarName == VarAName) { VarB = &V; break; }
		}
		UNTEST_ASSERT_TRUE(VarB != nullptr);
		UNTEST_EXPECT_EQ(VarA.PropertyFlags, VarB->PropertyFlags);
		UNTEST_EXPECT_EQ(VarA.Category.ToString(), VarB->Category.ToString());
		UNTEST_EXPECT_EQ(VarA.RepNotifyFunc, VarB->RepNotifyFunc);
		UNTEST_EXPECT_EQ(static_cast<int32>(VarA.ReplicationCondition),
		                 static_cast<int32>(VarB->ReplicationCondition));
		UNTEST_EXPECT_TRUE(VarA.VarType == VarB->VarType);
	}

	CleanupTestAsset(ApplySpecTestBPParityPath_A);
	CleanupTestAsset(ApplySpecTestBPParityPath_B);
	co_return;
}

#endif // WITH_UNTESTED
