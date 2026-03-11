// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonTool_BehaviorTreeInspect.h"
#include "Tools/ClaireonTool_BehaviorTreeInspectBlackboard.h"
#include "Tools/ClaireonTool_BehaviorTreeEdit.h"
#include "Tools/ClaireonTool_EQSInspect.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Test asset paths
// ---------------------------------------------------------------------------
static const TCHAR* TestBTPath = TEXT("/Game/BP/AI/BT/BT_CombatAttacking_Default");
static const TCHAR* TestBBPath = TEXT("/Game/BP/AI/BT/BB_AI_Default");
static const TCHAR* TestEQSPath = TEXT("/Game/BP/AI/EQS/EQS_CombatWaiting_Strafe");

// ---------------------------------------------------------------------------
// Helper: Extract session ID from structured FToolResult Data
// ---------------------------------------------------------------------------
namespace
{
	FString ExtractSessionIdFromBTResult(const IClaireonTool::FToolResult& Result)
	{
		if (Result.bIsError || !Result.Data.IsValid())
		{
			return FString();
		}
		FString SessionId;
		Result.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}
}

// ============================================================================
// editor.behaviortree.inspect
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BehaviorTree, InspectMissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_BehaviorTreeInspect Tool;

	// Missing asset_path should error
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("asset_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BehaviorTree, InspectBadAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_BehaviorTreeInspect Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TEXT("/Game/DoesNotExist/BT_Fake"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Failed to load")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BehaviorTree, InspectSummary, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BehaviorTreeInspect Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestBTPath);
	Args->SetStringField(TEXT("detail_level"), TEXT("summary"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Structure text is in Data["structure"]
	FString Structure;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("structure"), Structure));

	// Must contain the tree header
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("=== Behavior Tree:")));
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("BT_CombatAttacking_Default")));

	// Must have a root node and some structure
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("root")));

	// Must have blackboard key usage section
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("=== Blackboard Key Usage ===")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BehaviorTree, InspectFull, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BehaviorTreeInspect Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestBTPath);
	Args->SetStringField(TEXT("detail_level"), TEXT("full"));
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Structure text is in Data["structure"]
	FString Structure;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("structure"), Structure));

	// Full detail should be longer than summary
	UNTEST_EXPECT_TRUE(Structure.Len() > 100);

	// Should contain tree structure elements
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("=== Behavior Tree:")));
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("BT_CombatAttacking_Default")));

	// Full detail includes node descriptions — check for at least one node type marker
	UNTEST_EXPECT_TRUE(
		Structure.Contains(TEXT("[Task]")) ||
		Structure.Contains(TEXT("[Selector]")) ||
		Structure.Contains(TEXT("[Sequence]")) ||
		Structure.Contains(TEXT("[Service]")) ||
		Structure.Contains(TEXT("[Decorator]")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BehaviorTree, InspectDefaultsToFull, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BehaviorTreeInspect Tool;

	// No detail_level specified — should default to full
	TSharedPtr<FJsonObject> ArgsDefault = MakeShared<FJsonObject>();
	ArgsDefault->SetStringField(TEXT("asset_path"), TestBTPath);
	auto ResultDefault = Tool.Execute(ArgsDefault);
	UNTEST_ASSERT_FALSE(ResultDefault.bIsError);

	TSharedPtr<FJsonObject> ArgsFull = MakeShared<FJsonObject>();
	ArgsFull->SetStringField(TEXT("asset_path"), TestBTPath);
	ArgsFull->SetStringField(TEXT("detail_level"), TEXT("full"));
	auto ResultFull = Tool.Execute(ArgsFull);
	UNTEST_ASSERT_FALSE(ResultFull.bIsError);

	// Structure content should be identical when detail_level is omitted vs explicit "full"
	FString DefaultStructure, FullStructure;
	ResultDefault.Data->TryGetStringField(TEXT("structure"), DefaultStructure);
	ResultFull.Data->TryGetStringField(TEXT("structure"), FullStructure);
	UNTEST_EXPECT_STREQ(DefaultStructure, FullStructure);

	co_return;
}

// ============================================================================
// editor.behaviortree.inspectBlackboard
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, Blackboard, InspectMissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_BehaviorTreeInspectBlackboard Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("asset_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Blackboard, InspectBBDefault, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BehaviorTreeInspectBlackboard Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestBBPath);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Structure text is in Data["structure"]
	FString Structure;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("structure"), Structure));

	// Must contain the blackboard header
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("=== Blackboard:")));
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("BB_AI_Default")));

	// Must have the keys section
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("--- Keys ---")));

	// Must show total keys count
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("Total keys:")));

	// Structured data must have key counts
	double OwnKeyCount = 0.0;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetNumberField(TEXT("own_key_count"), OwnKeyCount));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Blackboard, InspectSummaryVsFull, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BehaviorTreeInspectBlackboard Tool;

	TSharedPtr<FJsonObject> ArgsSummary = MakeShared<FJsonObject>();
	ArgsSummary->SetStringField(TEXT("asset_path"), TestBBPath);
	ArgsSummary->SetStringField(TEXT("detail_level"), TEXT("summary"));
	auto ResultSummary = Tool.Execute(ArgsSummary);
	UNTEST_ASSERT_FALSE(ResultSummary.bIsError);

	TSharedPtr<FJsonObject> ArgsFull = MakeShared<FJsonObject>();
	ArgsFull->SetStringField(TEXT("asset_path"), TestBBPath);
	ArgsFull->SetStringField(TEXT("detail_level"), TEXT("full"));
	auto ResultFull = Tool.Execute(ArgsFull);
	UNTEST_ASSERT_FALSE(ResultFull.bIsError);

	// Full detail should have at least as much structure text as summary
	FString SummaryStructure, FullStructure;
	ResultSummary.Data->TryGetStringField(TEXT("structure"), SummaryStructure);
	ResultFull.Data->TryGetStringField(TEXT("structure"), FullStructure);
	UNTEST_EXPECT_TRUE(FullStructure.Len() >= SummaryStructure.Len());

	co_return;
}

// ============================================================================
// editor.eqs.inspect
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EQS, InspectMissingAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_EQSInspect Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("asset_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EQS, InspectStrafe, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_EQSInspect Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestEQSPath);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Structure text is in Data["structure"]
	FString Structure;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("structure"), Structure));

	// Must contain the EQS header
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("=== EQS Query:")));
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("EQS_CombatWaiting_Strafe")));

	// Must have at least one option
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("--- Option")));

	// Must have a generator
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("[Generator]")));

	// Must have context classes section
	UNTEST_EXPECT_TRUE(Structure.Contains(TEXT("=== Context Classes Referenced ===")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EQS, InspectFullIncludesProperties, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_EQSInspect Tool;

	TSharedPtr<FJsonObject> ArgsSummary = MakeShared<FJsonObject>();
	ArgsSummary->SetStringField(TEXT("asset_path"), TestEQSPath);
	ArgsSummary->SetStringField(TEXT("detail_level"), TEXT("summary"));
	auto ResultSummary = Tool.Execute(ArgsSummary);
	UNTEST_ASSERT_FALSE(ResultSummary.bIsError);

	TSharedPtr<FJsonObject> ArgsFull = MakeShared<FJsonObject>();
	ArgsFull->SetStringField(TEXT("asset_path"), TestEQSPath);
	ArgsFull->SetStringField(TEXT("detail_level"), TEXT("full"));
	auto ResultFull = Tool.Execute(ArgsFull);
	UNTEST_ASSERT_FALSE(ResultFull.bIsError);

	// Full detail should have at least as much structure text as summary
	FString SummaryStructure, FullStructure;
	ResultSummary.Data->TryGetStringField(TEXT("structure"), SummaryStructure);
	ResultFull.Data->TryGetStringField(TEXT("structure"), FullStructure);
	UNTEST_EXPECT_TRUE(FullStructure.Len() >= SummaryStructure.Len());

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EQS, InspectWrongAssetType, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_EQSInspect Tool;

	// Pass a BT asset path to an EQS tool — should give a type mismatch error
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), TestBTPath);
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("not an EQS Query")));
	co_return;
}

// ============================================================================
// editor.behaviortree.edit — open/close/status session lifecycle
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BehaviorTreeEdit, MissingOperation, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_BehaviorTreeEdit Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("operation")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BehaviorTreeEdit, OpenRequiresAssetPath, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_BehaviorTreeEdit Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("operation"), TEXT("open"));
	// No params.asset_path
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("asset_path")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BehaviorTreeEdit, OpenCloseStatusCycle, UNTEST_TIMEOUTMS(15000))
{
	ClaireonTool_BehaviorTreeEdit Tool;

	// --- Step 1: Open a session ---
	TSharedPtr<FJsonObject> OpenParams = MakeShared<FJsonObject>();
	OpenParams->SetStringField(TEXT("asset_path"), TestBTPath);

	TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
	OpenArgs->SetStringField(TEXT("operation"), TEXT("open"));
	OpenArgs->SetObjectField(TEXT("params"), OpenParams);

	auto OpenResult = Tool.Execute(OpenArgs);
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	UNTEST_ASSERT_PTR(OpenResult.Data.Get());

	// Session ID and asset path are in structured Data
	FString SessionId = ExtractSessionIdFromBTResult(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	FString OpenedAssetPath;
	UNTEST_EXPECT_TRUE(OpenResult.Data->TryGetStringField(TEXT("asset_path"), OpenedAssetPath));
	UNTEST_EXPECT_TRUE(OpenedAssetPath.Contains(TEXT("BT_CombatAttacking_Default")));

	// --- Step 2: Status check ---
	TSharedPtr<FJsonObject> StatusArgs = MakeShared<FJsonObject>();
	StatusArgs->SetStringField(TEXT("operation"), TEXT("status"));
	StatusArgs->SetStringField(TEXT("session_id"), SessionId);

	auto StatusResult = Tool.Execute(StatusArgs);
	UNTEST_ASSERT_FALSE(StatusResult.bIsError);
	UNTEST_ASSERT_PTR(StatusResult.Data.Get());

	// Status result contains session_id in Data
	FString StatusSessionId;
	UNTEST_EXPECT_TRUE(StatusResult.Data->TryGetStringField(TEXT("session_id"), StatusSessionId));
	UNTEST_EXPECT_STREQ(StatusSessionId, SessionId);

	// --- Step 3: List sessions should show our session ---
	TSharedPtr<FJsonObject> ListArgs = MakeShared<FJsonObject>();
	ListArgs->SetStringField(TEXT("operation"), TEXT("list_sessions"));

	auto ListResult = Tool.Execute(ListArgs);
	UNTEST_ASSERT_FALSE(ListResult.bIsError);
	UNTEST_ASSERT_PTR(ListResult.Data.Get());

	// Check session count >= 1
	double SessionCount = 0.0;
	UNTEST_EXPECT_TRUE(ListResult.Data->TryGetNumberField(TEXT("session_count"), SessionCount));
	UNTEST_EXPECT_TRUE(SessionCount >= 1.0);

	// --- Step 4: Close the session ---
	TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
	CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
	CloseArgs->SetStringField(TEXT("session_id"), SessionId);

	auto CloseResult = Tool.Execute(CloseArgs);
	UNTEST_ASSERT_FALSE(CloseResult.bIsError);
	UNTEST_EXPECT_TRUE(CloseResult.GetContentAsString().Contains(TEXT("Session closed")));

	// --- Step 5: Status on closed session should fail ---
	auto StatusAfterClose = Tool.Execute(StatusArgs);
	UNTEST_ASSERT_TRUE(StatusAfterClose.bIsError);
	UNTEST_EXPECT_TRUE(StatusAfterClose.GetContentAsString().Contains(TEXT("not found or expired")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BehaviorTreeEdit, OpenDuplicateBlockedByLock, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BehaviorTreeEdit Tool;

	TSharedPtr<FJsonObject> OpenParams = MakeShared<FJsonObject>();
	OpenParams->SetStringField(TEXT("asset_path"), TestBTPath);

	TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
	OpenArgs->SetStringField(TEXT("operation"), TEXT("open"));
	OpenArgs->SetObjectField(TEXT("params"), OpenParams);

	auto Result1 = Tool.Execute(OpenArgs);
	UNTEST_ASSERT_FALSE(Result1.bIsError);

	// Second open should be blocked by asset lock
	auto Result2 = Tool.Execute(OpenArgs);
	UNTEST_EXPECT_TRUE(Result2.bIsError);
	UNTEST_EXPECT_TRUE(Result2.GetContentAsString().Contains(TEXT("locked")));

	// Clean up using structured session ID from Data
	FString SessionId1 = ExtractSessionIdFromBTResult(Result1);
	if (!SessionId1.IsEmpty())
	{
		TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
		CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
		CloseArgs->SetStringField(TEXT("session_id"), SessionId1);
		Tool.Execute(CloseArgs);
	}

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BehaviorTreeEdit, OperationsRequireParamsOnMissingInput, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_BehaviorTreeEdit Tool;

	// Open a session first
	TSharedPtr<FJsonObject> OpenParams = MakeShared<FJsonObject>();
	OpenParams->SetStringField(TEXT("asset_path"), TestBTPath);

	TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
	OpenArgs->SetStringField(TEXT("operation"), TEXT("open"));
	OpenArgs->SetObjectField(TEXT("params"), OpenParams);

	auto OpenResult = Tool.Execute(OpenArgs);
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);

	// Extract session ID from structured Data
	FString SessionId = ExtractSessionIdFromBTResult(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	// Calling operations without required params should return errors (not crash)
	const TArray<FString> Ops = {
		TEXT("add_node"), TEXT("remove_node"), TEXT("move_node"),
		TEXT("set_node_property"), TEXT("add_decorator"), TEXT("remove_decorator"),
		TEXT("add_service"), TEXT("remove_service")
	};

	for (const FString& Op : Ops)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), Op);
		Args->SetStringField(TEXT("session_id"), SessionId);
		// No params — should error gracefully

		auto Result = Tool.Execute(Args);
		UNTEST_EXPECT_TRUE(Result.bIsError);
		// Should mention missing parameter
		UNTEST_EXPECT_TRUE(Result.GetContentAsString().Contains(TEXT("Missing")) || Result.GetContentAsString().Contains(TEXT("required")));
	}

	// update_asset and save should work without extra params
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("update_asset"));
		Args->SetStringField(TEXT("session_id"), SessionId);
		auto Result = Tool.Execute(Args);
		UNTEST_EXPECT_FALSE(Result.bIsError);
	}

	// Cleanup
	TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
	CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
	CloseArgs->SetStringField(TEXT("session_id"), SessionId);
	Tool.Execute(CloseArgs);

	co_return;
}

// ============================================================================
// Schema validation — verify tools return valid schemas
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BehaviorTree, InspectSchemaValid, UNTEST_TIMEOUTMS(1000))
{
	ClaireonTool_BehaviorTreeInspect Tool;
	UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("inspect_behaviortree"));
	UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());

	auto Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_PTR(Schema.Get());

	FString Type;
	UNTEST_EXPECT_TRUE(Schema->TryGetStringField(TEXT("type"), Type));
	UNTEST_EXPECT_STREQ(Type, TEXT("object"));

	const TSharedPtr<FJsonObject>* Props;
	UNTEST_EXPECT_TRUE(Schema->TryGetObjectField(TEXT("properties"), Props));

	const TArray<TSharedPtr<FJsonValue>>* Required;
	UNTEST_EXPECT_TRUE(Schema->TryGetArrayField(TEXT("required"), Required));
	UNTEST_EXPECT_TRUE(Required->Num() >= 1);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, Blackboard, InspectSchemaValid, UNTEST_TIMEOUTMS(1000))
{
	ClaireonTool_BehaviorTreeInspectBlackboard Tool;
	UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("editor.behaviortree.inspectBlackboard"));
	UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());

	auto Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_PTR(Schema.Get());

	FString Type;
	UNTEST_EXPECT_TRUE(Schema->TryGetStringField(TEXT("type"), Type));
	UNTEST_EXPECT_STREQ(Type, TEXT("object"));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, EQS, InspectSchemaValid, UNTEST_TIMEOUTMS(1000))
{
	ClaireonTool_EQSInspect Tool;
	UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("inspect_eqs"));
	UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());

	auto Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_PTR(Schema.Get());

	FString Type;
	UNTEST_EXPECT_TRUE(Schema->TryGetStringField(TEXT("type"), Type));
	UNTEST_EXPECT_STREQ(Type, TEXT("object"));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BehaviorTreeEdit, SchemaValid, UNTEST_TIMEOUTMS(1000))
{
	ClaireonTool_BehaviorTreeEdit Tool;
	UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("edit_behaviortree"));
	UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());

	auto Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_PTR(Schema.Get());

	FString Type;
	UNTEST_EXPECT_TRUE(Schema->TryGetStringField(TEXT("type"), Type));
	UNTEST_EXPECT_STREQ(Type, TEXT("object"));

	const TSharedPtr<FJsonObject>* Props;
	UNTEST_EXPECT_TRUE(Schema->TryGetObjectField(TEXT("properties"), Props));

	// Must have operation, session_id, and params properties
	const TSharedPtr<FJsonObject>* OpProp;
	UNTEST_EXPECT_TRUE((*Props)->TryGetObjectField(TEXT("operation"), OpProp));

	const TSharedPtr<FJsonObject>* SessionProp;
	UNTEST_EXPECT_TRUE((*Props)->TryGetObjectField(TEXT("session_id"), SessionProp));

	const TSharedPtr<FJsonObject>* ParamsProp;
	UNTEST_EXPECT_TRUE((*Props)->TryGetObjectField(TEXT("params"), ParamsProp));

	co_return;
}


#endif // WITH_UNTESTED
