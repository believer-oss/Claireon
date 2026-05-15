// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for apply_spec operation across all 8 session-based tools.
// Each test generates an asset from a declarative spec, then inspects it
// to verify the entities were created correctly.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"

// Edit tools (apply_spec targets -- decomposed per-system tools)
#include "Tools/ClaireonBehaviorTreeTool_ApplySpec.h"
#include "Tools/ClaireonBlueprintGraphTool_ApplySpec.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonStateTreeTool_ApplySpec.h"
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
#include "ObjectTools.h"

// ---------------------------------------------------------------------------
// Test asset paths -- existing game assets used for apply_spec tests
// ---------------------------------------------------------------------------
static const TCHAR* ApplySpecTestBTPath        = TEXT("/Game/BP/AI/BT/BT_CombatAttacking_Default");
static const TCHAR* ApplySpecTestBBPath        = TEXT("/Game/BP/AI/BT/BB_AI_Default");
static const TCHAR* ApplySpecTestEQSPath       = TEXT("/Game/BP/AI/EQS/EQS_CombatWaiting_Strafe");
static const TCHAR* ApplySpecTestBPPath        = TEXT("/Game/__MCPTests/BP_ApplySpecTest");
static const TCHAR* ApplySpecTestSTPath        = TEXT("/Game/BP/AI/ST/ST_TestDummy");
static const TCHAR* ApplySpecTestNiagaraPath   = TEXT("/Game/Art_Lib/VOL/NS_LocalVolumeFog");
static const TCHAR* ApplySpecTestWidgetPath    = TEXT("/Game/__MCPTests/WBP_ApplySpecTest");
static const TCHAR* ApplySpecTestPCGPath       = TEXT("/Game/__MCPTests/PCG_ApplySpecTest");

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace
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

void CleanupTestAsset(const FString& AssetPath)
{
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (Asset)
	{
		TArray<UObject*> AssetsToDelete;
		AssetsToDelete.Add(Asset);
		ObjectTools::ForceDeleteObjects(AssetsToDelete, false);
	}
}

} // anonymous namespace

// ============================================================================
// BehaviorTree -- apply_spec creates nodes in an existing BT
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_BehaviorTree, CreateNodesFromSpec, UNTEST_TIMEOUTMS(30000))
{
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
	FString InspectText = InspectResult.GetContentAsString();
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
	UNTEST_EXPECT_TRUE(InspectResult.GetContentAsString().Contains(TEXT("PrintString")));

	CleanupTestAsset(ApplySpecTestBPPath);
	co_return;
}

// ============================================================================
// Blackboard -- apply_spec creates keys
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_Blackboard, CreateKeysFromSpec, UNTEST_TIMEOUTMS(30000))
{
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
	FString InspectText = InspectResult.GetContentAsString();
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
	UNTEST_EXPECT_TRUE(InspectResult.GetContentAsString().Contains(TEXT("SimpleGrid")));

	co_return;
}

// ============================================================================
// StateTree -- apply_spec creates states
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_StateTree, CreateStatesFromSpec, UNTEST_TIMEOUTMS(30000))
{
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
	FString InspectText = InspectResult.GetContentAsString();
	UNTEST_EXPECT_TRUE(InspectText.Contains(TEXT("TestIdle")));
	UNTEST_EXPECT_TRUE(InspectText.Contains(TEXT("TestCombat")));

	co_return;
}

// ============================================================================
// Niagara -- apply_spec adds parameters to an existing system
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_Niagara, CreateParametersFromSpec, UNTEST_TIMEOUTMS(30000))
{
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
		P->SetStringField(TEXT("type"), TEXT("FLinearColor"));
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

	// Verify via inspect
	ClaireonTool_NiagaraInspect InspectTool;
	auto InspectResult = InspectTool.Execute(MakeInspectArgs(ApplySpecTestNiagaraPath));
	UNTEST_ASSERT_FALSE(InspectResult.bIsError);
	UNTEST_EXPECT_TRUE(InspectResult.GetContentAsString().Contains(TEXT("TestIntensity")));

	co_return;
}

// ============================================================================
// PCG -- apply_spec creates nodes and connections
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplySpec_PCG, CreateNodesFromSpec, UNTEST_TIMEOUTMS(30000))
{
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

	// PCG test asset might not exist -- handle gracefully
	if (Result.bIsError && Result.GetContentAsString().Contains(TEXT("Failed to open")))
	{
		UE_LOG(LogTemp, Warning, TEXT("[ApplySpec_PCG] Test asset not found at %s, skipping"), ApplySpecTestPCGPath);
		co_return;
	}

	UNTEST_ASSERT_TRUE(VerifyApplySpecResult(Result, 2));

	const TSharedPtr<FJsonObject>* Mappings = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("id_mappings"), Mappings));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("surf_sampler")));
	UNTEST_EXPECT_TRUE((*Mappings)->HasField(TEXT("static_mesh")));

	// Verify via inspect
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
	FString InspectText = InspectResult.GetContentAsString();
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

#endif // WITH_UNTESTED
