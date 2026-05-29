// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Regression tests for bp_apply_delta -- in particular, that
// CallFunction nodes created through the batch path populate their pins
// correctly.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonTool_ApplyBlueprintDelta.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Tools/ClaireonBlueprintGraphTool_AddVariable.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h"
#include "ClaireonBlueprintHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_AsyncAction.h"
#include "ObjectTools.h"

// ---------------------------------------------------------------------------
// Test asset path + helpers
// ---------------------------------------------------------------------------
static const TCHAR* ApplyBPDeltaPinsTestPath = TEXT("/Game/__MCPTests/BP_ApplyBlueprintGraphPinsTest");

namespace
{

void ApplyGraphTests_CleanupTestAsset(const FString& AssetPath)
{
	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (Asset)
	{
		TArray<UObject*> AssetsToDelete;
		AssetsToDelete.Add(Asset);
		ObjectTools::ForceDeleteObjects(AssetsToDelete, false);
	}
}

// Create a scratch BP via ClaireonBlueprintGraphTool_Create + open a session via
// ClaireonBlueprintGraphTool_Open. Returns the session_id string; empty string
// on failure.  ParentClass defaults to "Actor" (matches the card's repro class).
FString OpenTestSession(const TCHAR* AssetPath, const TCHAR* ParentClass = TEXT("Actor"))
{
	{
		ClaireonBlueprintGraphTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("parent_class"), ParentClass);
		auto R = CreateTool.Execute(Args);
		if (R.bIsError) return FString();
	}
	ClaireonBlueprintGraphTool_Open OpenTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	auto R = OpenTool.Execute(Args);
	if (R.bIsError || !R.Data.IsValid()) return FString();
	FString SessionId;
	R.Data->TryGetStringField(TEXT("session_id"), SessionId);
	return SessionId;
}

TSharedPtr<FJsonValue> ApplyGraphTests_MakeObj(const TSharedPtr<FJsonObject>& Obj)
{
	return MakeShared<FJsonValueObject>(Obj);
}

// Build an apply_delta args object with a single node entry.
TSharedPtr<FJsonObject> MakeApplyDeltaArgsSingle(const FString& SessionId, const TSharedPtr<FJsonObject>& Node)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(ApplyGraphTests_MakeObj(Node));
	Args->SetArrayField(TEXT("nodes"), Nodes);
	return Args;
}

// Look up the created node in the session graph by id_map local id.
UEdGraphNode* ResolveCreatedNode(const IClaireonTool::FToolResult& Result, const FString& LocalId, const FString& SessionId)
{
	if (!Result.Data.IsValid()) return nullptr;

	const TSharedPtr<FJsonObject>* IdMap = nullptr;
	if (!Result.Data->TryGetObjectField(TEXT("id_map"), IdMap) || !IdMap || !(*IdMap).IsValid()) return nullptr;

	FString GuidStr;
	if (!(*IdMap)->TryGetStringField(LocalId, GuidStr) || GuidStr.IsEmpty()) return nullptr;

	FGuid NodeGuid;
	if (!FGuid::Parse(GuidStr, NodeGuid)) return nullptr;

	FBlueprintEditToolData* Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
	if (!Data) return nullptr;
	UEdGraph* Graph = Data->Graph.Get();
	if (!Graph) return nullptr;

	return ClaireonBlueprintHelpers::FindNodeByGuid(Graph, NodeGuid);
}

// True if a pin matching PinName (case-insensitive) exists on Node.
bool NodeHasPin(UEdGraphNode* Node, const TCHAR* PinName)
{
	if (!Node) return false;
	for (UEdGraphPin* Pin : Node->Pins)
	{
		if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

} // anonymous namespace

// ============================================================================
// KismetSystemLibrary.PrintString -- classic canary: a standard static helper
// that pre-fix would come through with just the exec in/out pins.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplyBlueprintDelta_Pins, KismetSystemLibrary_PrintString_HasAllPins, UNTEST_TIMEOUTMS(30000))
{
	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	FString SessionId = OpenTestSession(ApplyBPDeltaPinsTestPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("id"), TEXT("print1"));
	Node->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	Node->SetStringField(TEXT("function_name"), TEXT("PrintString"));
	Node->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));

	ClaireonTool_ApplyBlueprintDelta Tool;
	auto Result = Tool.Execute(MakeApplyDeltaArgsSingle(SessionId, Node));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	UEdGraphNode* Created = ResolveCreatedNode(Result, TEXT("print1"), SessionId);
	UNTEST_ASSERT_PTR(Created);

	// PrintString has: execute, then, WorldContextObject, InString, bPrintToScreen, bPrintToLog, ...
	UNTEST_EXPECT_GT(Created->Pins.Num(), 1);
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("execute")));
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("then")));
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("InString")));

	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	co_return;
}

// ============================================================================
// AsyncAction auto-pick. The helper picks UK2Node_AsyncAction, the call site
// calls InitializeProxyFromFunction, and AllocateDefaultPins emits the
// delegate exec pins from the proxy class. A plain UK2Node_CallFunction would
// miss the BlueprintAssignable OnComplete exec pin.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplyBlueprintDelta_Pins,
    FlowprintAwait_Delay_AsyncAction_HasOnComplete, UNTEST_TIMEOUTMS(30000))
{
	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	FString SessionId = OpenTestSession(ApplyBPDeltaPinsTestPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Custom latent-factory referenced by string only -- no #include, no Build.cs edge.
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("id"), TEXT("delay1"));
	Node->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	Node->SetStringField(TEXT("function_name"), TEXT("AwaitDelay"));
	Node->SetStringField(TEXT("function_class"),
		TEXT("/Script/Engine.AsyncActionLoadPrimaryAsset"));

	ClaireonTool_ApplyBlueprintDelta Tool;
	auto Result = Tool.Execute(MakeApplyDeltaArgsSingle(SessionId, Node));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	UEdGraphNode* Created = ResolveCreatedNode(Result, TEXT("delay1"), SessionId);
	UNTEST_ASSERT_PTR(Created);

	// Cast check: regression here means the helper or call-site branch
	// failed to route to the AsyncAction path.
	UK2Node_AsyncAction* AsyncNode = Cast<UK2Node_AsyncAction>(Created);
	UNTEST_ASSERT_PTR(AsyncNode);

	// Card-defining pin: the BlueprintAssignable delegate exec output.
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("OnComplete")));

	// Proxy-class parameter pin (factory's float arg).
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("Duration")));

	// Standard exec pair -- confirms the base wiring.
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("execute")));
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("then")));

	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	co_return;
}

// ============================================================================
// AsyncAction typed branch. The node_type='AsyncAction' surface in
// ClaireonBlueprintNodeFactory::CreateNode constructs the same
// UK2Node_AsyncAction the CallFunction helper-detection path produces, but
// without requiring node_type='CallFunction' + the four-conjunct helper
// guard. Callers who already know they want an async node state intent
// directly.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplyBlueprintDelta_Pins,
    AsyncActionNodeType_HasOnComplete, UNTEST_TIMEOUTMS(30000))
{
	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	FString SessionId = OpenTestSession(ApplyBPDeltaPinsTestPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Custom latent-factory referenced by string only -- no #include, no Build.cs edge.
	// Differs from the CallFunction-helper-detection sibling test ONLY in
	// node_type: 'AsyncAction' instead of 'CallFunction'.
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("id"), TEXT("delay1"));
	Node->SetStringField(TEXT("node_type"), TEXT("AsyncAction"));
	Node->SetStringField(TEXT("function_name"), TEXT("AwaitDelay"));
	Node->SetStringField(TEXT("function_class"),
		TEXT("/Script/Engine.AsyncActionLoadPrimaryAsset"));

	ClaireonTool_ApplyBlueprintDelta Tool;
	auto Result = Tool.Execute(MakeApplyDeltaArgsSingle(SessionId, Node));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	UEdGraphNode* Created = ResolveCreatedNode(Result, TEXT("delay1"), SessionId);
	UNTEST_ASSERT_PTR(Created);

	// Cast check: regression here means the new typed branch failed to route
	// to UK2Node_AsyncAction (e.g., fell through to the Generic fallback or
	// emitted a CallFunction instead).
	UK2Node_AsyncAction* AsyncNode = Cast<UK2Node_AsyncAction>(Created);
	UNTEST_ASSERT_PTR(AsyncNode);

	// Card-defining pin: the BlueprintAssignable delegate exec output. This is
	// the pin AllocateDefaultPins synthesizes from the proxy class's
	// multicast delegate; its presence proves InitializeProxyFromFunction
	// populated ProxyFactoryClass / ProxyClass / ProxyFactoryFunctionName.
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("OnComplete")));

	// Proxy-class parameter pin (factory's float arg).
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("Duration")));

	// Standard exec pair -- confirms the base wiring.
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("execute")));
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("then")));

	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	co_return;
}

// ============================================================================
// Generic + K2Node_AsyncAction without node_properties.
// Pre-fix, this produced the inert "Async Task: Missing Function" stub
// silently (exec pins only, no delegate output). Post-fix, the loud-failure
// guard in ClaireonBlueprintNodeFactory::CreateNode catches the half-populated
// proxy bag and returns an error that points the caller at the
// node_type='AsyncAction' surface.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplyBlueprintDelta_Pins,
    GenericK2NodeAsyncAction_WithoutProxyBag_FailsLoudly, UNTEST_TIMEOUTMS(30000))
{
	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	FString SessionId = OpenTestSession(ApplyBPDeltaPinsTestPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Generic surface with class_name='K2Node_AsyncAction' and NO
	// node_properties -- the silent-stub failure mode the card identifies.
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("id"), TEXT("stub1"));
	Node->SetStringField(TEXT("node_type"), TEXT("Generic"));
	Node->SetStringField(TEXT("class_name"), TEXT("K2Node_AsyncAction"));

	ClaireonTool_ApplyBlueprintDelta Tool;
	auto Result = Tool.Execute(MakeApplyDeltaArgsSingle(SessionId, Node));

	// The factory's loud-failure guard must short-circuit before
	// Graph->AddNode runs. bIsError must be true.
	UNTEST_ASSERT_TRUE(Result.bIsError);

	// Error message must contain both the recommended surface name
	// ('AsyncAction') and at least one of the proxy-field names
	// ('ProxyFactoryFunctionName') so the caller can self-locate the fix.
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("AsyncAction")));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("ProxyFactoryFunctionName")));

	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	co_return;
}

// ============================================================================
// Self-bound CallFunction -- primary card repro.  Before T2+T3 this node
// would end up with zero pins because the factory never resolved the UFunction
// and never triggered ReconstructNode.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplyBlueprintDelta_Pins, Actor_SetHiddenInGame_SelfBound_HasPins, UNTEST_TIMEOUTMS(30000))
{
	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	FString SessionId = OpenTestSession(ApplyBPDeltaPinsTestPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// SetLifeSpan is a self-bound BlueprintCallable on AActor; no function_class.
	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("id"), TEXT("setls1"));
	Node->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	Node->SetStringField(TEXT("function_name"), TEXT("SetLifeSpan"));

	ClaireonTool_ApplyBlueprintDelta Tool;
	auto Result = Tool.Execute(MakeApplyDeltaArgsSingle(SessionId, Node));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	UEdGraphNode* Created = ResolveCreatedNode(Result, TEXT("setls1"), SessionId);
	UNTEST_ASSERT_PTR(Created);

	// Expect exec in + exec out + self + InLifespan.
	UNTEST_EXPECT_GT(Created->Pins.Num(), 1);
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("execute")));
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("then")));
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("InLifespan")));

	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	co_return;
}

// ============================================================================
// Sequence -- negative-control test.  Factory already handles Sequence
// correctly; this catches accidental regressions in the shared tail.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplyBlueprintDelta_Pins, Sequence_HasThen0_Then1, UNTEST_TIMEOUTMS(30000))
{
	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	FString SessionId = OpenTestSession(ApplyBPDeltaPinsTestPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("id"), TEXT("seq1"));
	Node->SetStringField(TEXT("node_type"), TEXT("Sequence"));

	ClaireonTool_ApplyBlueprintDelta Tool;
	auto Result = Tool.Execute(MakeApplyDeltaArgsSingle(SessionId, Node));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	UEdGraphNode* Created = ResolveCreatedNode(Result, TEXT("seq1"), SessionId);
	UNTEST_ASSERT_PTR(Created);

	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("execute")));
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("then_0")));
	UNTEST_EXPECT_TRUE(NodeHasPin(Created, TEXT("then_1")));

	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	co_return;
}

// ============================================================================
// VariableGet -- FProperty-driven path.  AllocateDefaultPins already builds
// the pin, so we just verify it comes through with the expected typed pin.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplyBlueprintDelta_Pins, VariableGet_TypedOutput, UNTEST_TIMEOUTMS(30000))
{
	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	FString SessionId = OpenTestSession(ApplyBPDeltaPinsTestPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Add a float variable to the blueprint first.
	{
		ClaireonBlueprintGraphTool_AddVariable AddVarTool;
		TSharedPtr<FJsonObject> VarArgs = MakeShared<FJsonObject>();
		VarArgs->SetStringField(TEXT("session_id"), SessionId);
		VarArgs->SetStringField(TEXT("variable_name"), TEXT("TestHealth"));
		VarArgs->SetStringField(TEXT("variable_type"), TEXT("float"));
		auto AddVarResult = AddVarTool.Execute(VarArgs);
		UNTEST_ASSERT_FALSE(AddVarResult.bIsError);
	}

	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("id"), TEXT("varget1"));
	Node->SetStringField(TEXT("node_type"), TEXT("VariableGet"));
	Node->SetStringField(TEXT("variable_name"), TEXT("TestHealth"));

	ClaireonTool_ApplyBlueprintDelta Tool;
	auto Result = Tool.Execute(MakeApplyDeltaArgsSingle(SessionId, Node));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	UEdGraphNode* Created = ResolveCreatedNode(Result, TEXT("varget1"), SessionId);
	UNTEST_ASSERT_PTR(Created);

	// Expect a single typed output pin with PC_Real (UE 5.5 float category).
	UEdGraphPin* OutPin = nullptr;
	for (UEdGraphPin* Pin : Created->Pins)
	{
		if (Pin && Pin->Direction == EGPD_Output && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			OutPin = Pin;
			break;
		}
	}
	UNTEST_ASSERT_PTR(OutPin);
	UNTEST_EXPECT_TRUE(OutPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Real);

	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	co_return;
}

// ============================================================================
// Bad function_class -- ensures the factory surfaces a resolution warning
// (was silent pre-fix) and that the node is still created but empty.
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ApplyBlueprintDelta_Pins, CallFunction_BadFunctionClass_EmitsWarning, UNTEST_TIMEOUTMS(30000))
{
	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	FString SessionId = OpenTestSession(ApplyBPDeltaPinsTestPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
	Node->SetStringField(TEXT("id"), TEXT("bad1"));
	Node->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	Node->SetStringField(TEXT("function_name"), TEXT("Anything"));
	Node->SetStringField(TEXT("function_class"), TEXT("NonexistentClassXYZ"));

	ClaireonTool_ApplyBlueprintDelta Tool;
	auto Result = Tool.Execute(MakeApplyDeltaArgsSingle(SessionId, Node));
	UNTEST_ASSERT_FALSE(Result.bIsError);

	bool bFoundWarning = false;
	for (const FString& W : Result.Warnings)
	{
		if (W.Contains(TEXT("NonexistentClassXYZ"), ESearchCase::IgnoreCase))
		{
			bFoundWarning = true;
			break;
		}
	}
	UNTEST_EXPECT_TRUE(bFoundWarning);

	UEdGraphNode* Created = ResolveCreatedNode(Result, TEXT("bad1"), SessionId);
	UNTEST_ASSERT_PTR(Created);

	// Function name was bogus too, so pin population should be minimal (no
	// function parameters were resolvable).  The node still exists -- the
	// fallback is SetSelfMember with no UFunction found.
	int32 NonExecPinCount = 0;
	for (UEdGraphPin* Pin : Created->Pins)
	{
		if (Pin && Pin->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
		{
			++NonExecPinCount;
		}
	}
	UNTEST_EXPECT_LE(NonExecPinCount, 1); // self pin is ok; function params are not

	ApplyGraphTests_CleanupTestAsset(ApplyBPDeltaPinsTestPath);
	co_return;
}

#endif // WITH_UNTESTED
