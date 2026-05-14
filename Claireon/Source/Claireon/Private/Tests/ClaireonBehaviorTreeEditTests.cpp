// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonBehaviorTreeTool_Open.h"
#include "Tools/ClaireonBehaviorTreeTool_Close.h"
#include "Tools/ClaireonBehaviorTreeTool_Status.h"
#include "Tools/ClaireonBehaviorTreeTool_ListNodeTypes.h"
#include "Tools/ClaireonBlackboardTool_Open.h"
#include "Tools/ClaireonBlackboardTool_Close.h"
#include "Tools/ClaireonBlackboardTool_Status.h"
#include "Tools/ClaireonEQSTool_Open.h"
#include "Tools/ClaireonEQSTool_Close.h"
#include "Tools/ClaireonEQSTool_Status.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

// ---------------------------------------------------------------------------
// Test asset paths
// ---------------------------------------------------------------------------
static const TCHAR* EditTestBTPath = TEXT("/Game/BP/AI/BT/BT_CombatAttacking_Default");
static const TCHAR* EditTestBBPath = TEXT("/Game/BP/AI/BT/BB_AI_Default");
static const TCHAR* EditTestEQSPath = TEXT("/Game/BP/AI/EQS/EQS_CombatWaiting_Strafe");

// ---------------------------------------------------------------------------
// Helper: Extract session ID from structured FToolResult Data
// ---------------------------------------------------------------------------
namespace
{
	FString MCPBehaviorTreeEditTests_ExtractSessionId(const IClaireonTool::FToolResult& Result)
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
// behaviortree_edit — Schema validation for updated tool
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BehaviorTreeEditV2, SchemaValid, UNTEST_TIMEOUTMS(1000))
{
	ClaireonBehaviorTreeTool_Open Tool;
	UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("behaviortree_open"));
	UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());

	auto Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_PTR(Schema.Get());

	FString Type;
	UNTEST_EXPECT_TRUE(Schema->TryGetStringField(TEXT("type"), Type));
	UNTEST_EXPECT_STREQ(Type, TEXT("object"));

	const TSharedPtr<FJsonObject>* Props;
	UNTEST_EXPECT_TRUE(Schema->TryGetObjectField(TEXT("properties"), Props));

	co_return;
}

// ============================================================================
// behaviortree_edit — Open/close with asset lock
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BehaviorTreeEditV2, OpenCloseCycle, UNTEST_TIMEOUTMS(15000))
{
	ClaireonBehaviorTreeTool_Open OpenTool;
	ClaireonBehaviorTreeTool_Status StatusTool;
	ClaireonBehaviorTreeTool_Close CloseTool;

	// Open
	TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
	OpenArgs->SetStringField(TEXT("asset_path"), EditTestBTPath);

	auto OpenResult = OpenTool.Execute(OpenArgs);
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	UNTEST_ASSERT_PTR(OpenResult.Data.Get());

	// Session ID and asset path are in structured Data
	FString SessionId;
	UNTEST_EXPECT_TRUE(OpenResult.Data->TryGetStringField(TEXT("session_id"), SessionId));
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	FString OpenedAssetPath;
	UNTEST_EXPECT_TRUE(OpenResult.Data->TryGetStringField(TEXT("asset_path"), OpenedAssetPath));
	UNTEST_EXPECT_TRUE(OpenedAssetPath.Contains(TEXT("BT_CombatAttacking_Default")));

	// Status
	TSharedPtr<FJsonObject> StatusArgs = MakeShared<FJsonObject>();
	StatusArgs->SetStringField(TEXT("session_id"), SessionId);

	auto StatusResult = StatusTool.Execute(StatusArgs);
	UNTEST_ASSERT_FALSE(StatusResult.bIsError);
	UNTEST_ASSERT_PTR(StatusResult.Data.Get());
	// session_id must be in Data
	FString StatusSessionId;
	UNTEST_EXPECT_TRUE(StatusResult.Data->TryGetStringField(TEXT("session_id"), StatusSessionId));
	UNTEST_EXPECT_STREQ(StatusSessionId, SessionId);

	// Close
	TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
	CloseArgs->SetStringField(TEXT("session_id"), SessionId);

	auto CloseResult = CloseTool.Execute(CloseArgs);
	UNTEST_ASSERT_FALSE(CloseResult.bIsError);

	co_return;
}

// ============================================================================
// behaviortree_edit — list_node_types discovery
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BehaviorTreeEditV2, ListNodeTypes, UNTEST_TIMEOUTMS(10000))
{
	ClaireonBehaviorTreeTool_ListNodeTypes Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("category"), TEXT("composite"));

	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_PTR(Result.Data.Get());

	// Node types text is in Data["node_types"]
	FString NodeTypesText;
	UNTEST_EXPECT_TRUE(Result.Data->TryGetStringField(TEXT("node_types"), NodeTypesText));
	UNTEST_EXPECT_TRUE(NodeTypesText.Contains(TEXT("Composite Nodes")));
	UNTEST_EXPECT_TRUE(NodeTypesText.Contains(TEXT("BTComposite_Selector")));

	co_return;
}

// ============================================================================
// behaviortree_edit — suppress_output
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BehaviorTreeEditV2, SuppressOutput, UNTEST_TIMEOUTMS(15000))
{
	ClaireonBehaviorTreeTool_Open OpenTool;
	ClaireonBehaviorTreeTool_Status StatusTool;
	ClaireonBehaviorTreeTool_Close CloseTool;

	// Open
	TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
	OpenArgs->SetStringField(TEXT("asset_path"), EditTestBTPath);

	auto OpenResult = OpenTool.Execute(OpenArgs);
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	FString SessionId = MCPBehaviorTreeEditTests_ExtractSessionId(OpenResult);
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	// Status with suppress_output
	TSharedPtr<FJsonObject> StatusArgs = MakeShared<FJsonObject>();
	StatusArgs->SetStringField(TEXT("session_id"), SessionId);
	StatusArgs->SetBoolField(TEXT("suppress_output"), true);

	auto SuppressedResult = StatusTool.Execute(StatusArgs);
	UNTEST_ASSERT_FALSE(SuppressedResult.bIsError);
	FString SuppressedOutput = SuppressedResult.GetContentAsString();
	UNTEST_EXPECT_TRUE(SuppressedOutput.StartsWith(TEXT("ok")));
	// Suppressed should be much shorter than full output
	UNTEST_EXPECT_TRUE(SuppressedOutput.Len() < 200);

	// Close
	TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
	CloseArgs->SetStringField(TEXT("session_id"), SessionId);
	CloseTool.Execute(CloseArgs);

	co_return;
}

// ============================================================================
// behaviortree_edit — Asset lock conflict
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BehaviorTreeEditV2, AssetLockConflict, UNTEST_TIMEOUTMS(15000))
{
	ClaireonBehaviorTreeTool_Open OpenTool;
	ClaireonBehaviorTreeTool_Close CloseTool;

	// Open first session
	TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
	OpenArgs->SetStringField(TEXT("asset_path"), EditTestBTPath);

	auto Result1 = OpenTool.Execute(OpenArgs);
	UNTEST_ASSERT_FALSE(Result1.bIsError);
	FString SessionId1 = MCPBehaviorTreeEditTests_ExtractSessionId(Result1);

	// Try to open second session on same asset — should fail with lock error
	auto Result2 = OpenTool.Execute(OpenArgs);
	UNTEST_ASSERT_TRUE(Result2.bIsError);
	UNTEST_EXPECT_TRUE(Result2.GetContentAsString().Contains(TEXT("locked")));

	// Cleanup
	TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
	CloseArgs->SetStringField(TEXT("session_id"), SessionId1);
	CloseTool.Execute(CloseArgs);

	co_return;
}

// ============================================================================
// blackboard_edit — Schema validation
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BlackboardEdit, SchemaValid, UNTEST_TIMEOUTMS(1000))
{
	ClaireonBlackboardTool_Open Tool;
	UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("blackboard_open"));
	UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());

	auto Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_PTR(Schema.Get());

	FString Type;
	UNTEST_EXPECT_TRUE(Schema->TryGetStringField(TEXT("type"), Type));
	UNTEST_EXPECT_STREQ(Type, TEXT("object"));

	co_return;
}

// ============================================================================
// blackboard_edit — Open/close cycle
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BlackboardEdit, OpenCloseCycle, UNTEST_TIMEOUTMS(15000))
{
	ClaireonBlackboardTool_Open OpenTool;
	ClaireonBlackboardTool_Status StatusTool;
	ClaireonBlackboardTool_Close CloseTool;

	// Open
	TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
	OpenArgs->SetStringField(TEXT("asset_path"), EditTestBBPath);

	auto OpenResult = OpenTool.Execute(OpenArgs);
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	UNTEST_ASSERT_PTR(OpenResult.Data.Get());

	// Session ID and asset path are in structured Data
	FString SessionId;
	UNTEST_EXPECT_TRUE(OpenResult.Data->TryGetStringField(TEXT("session_id"), SessionId));
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	FString OpenedAssetPath;
	UNTEST_EXPECT_TRUE(OpenResult.Data->TryGetStringField(TEXT("asset_path"), OpenedAssetPath));
	UNTEST_EXPECT_TRUE(OpenedAssetPath.Contains(TEXT("BB_AI_Default")));

	// Status
	TSharedPtr<FJsonObject> StatusArgs = MakeShared<FJsonObject>();
	StatusArgs->SetStringField(TEXT("session_id"), SessionId);

	auto StatusResult = StatusTool.Execute(StatusArgs);
	UNTEST_ASSERT_FALSE(StatusResult.bIsError);
	UNTEST_ASSERT_PTR(StatusResult.Data.Get());
	// Blackboard status result should contain asset_path referencing the blackboard
	FString StatusAssetPath;
	UNTEST_EXPECT_TRUE(StatusResult.Data->TryGetStringField(TEXT("asset_path"), StatusAssetPath));
	UNTEST_EXPECT_TRUE(StatusAssetPath.Contains(TEXT("BB_AI_Default")));

	// Close
	TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
	CloseArgs->SetStringField(TEXT("session_id"), SessionId);

	auto CloseResult = CloseTool.Execute(CloseArgs);
	UNTEST_ASSERT_FALSE(CloseResult.bIsError);

	co_return;
}

// ============================================================================
// eqs_edit — Schema validation
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EQSEdit, SchemaValid, UNTEST_TIMEOUTMS(1000))
{
	ClaireonEQSTool_Open Tool;
	UNTEST_EXPECT_STREQ(Tool.GetName(), TEXT("eqs_open"));
	UNTEST_EXPECT_TRUE(!Tool.GetDescription().IsEmpty());

	auto Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_PTR(Schema.Get());

	FString Type;
	UNTEST_EXPECT_TRUE(Schema->TryGetStringField(TEXT("type"), Type));
	UNTEST_EXPECT_STREQ(Type, TEXT("object"));

	co_return;
}

// ============================================================================
// eqs_edit — Open/close cycle
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, EQSEdit, OpenCloseCycle, UNTEST_TIMEOUTMS(15000))
{
	ClaireonEQSTool_Open OpenTool;
	ClaireonEQSTool_Status StatusTool;
	ClaireonEQSTool_Close CloseTool;

	// Open
	TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
	OpenArgs->SetStringField(TEXT("asset_path"), EditTestEQSPath);

	auto OpenResult = OpenTool.Execute(OpenArgs);
	UNTEST_ASSERT_FALSE(OpenResult.bIsError);
	UNTEST_ASSERT_PTR(OpenResult.Data.Get());

	// Session ID and asset path are in structured Data
	FString SessionId;
	UNTEST_EXPECT_TRUE(OpenResult.Data->TryGetStringField(TEXT("session_id"), SessionId));
	UNTEST_ASSERT_TRUE(!SessionId.IsEmpty());

	FString OpenedAssetPath;
	UNTEST_EXPECT_TRUE(OpenResult.Data->TryGetStringField(TEXT("asset_path"), OpenedAssetPath));
	UNTEST_EXPECT_TRUE(OpenedAssetPath.Contains(TEXT("EQS_CombatWaiting_Strafe")));

	// Status
	TSharedPtr<FJsonObject> StatusArgs = MakeShared<FJsonObject>();
	StatusArgs->SetStringField(TEXT("session_id"), SessionId);

	auto StatusResult = StatusTool.Execute(StatusArgs);
	UNTEST_ASSERT_FALSE(StatusResult.bIsError);
	UNTEST_ASSERT_PTR(StatusResult.Data.Get());
	// EQS status result should contain asset_path referencing the EQS query
	FString StatusAssetPath;
	UNTEST_EXPECT_TRUE(StatusResult.Data->TryGetStringField(TEXT("asset_path"), StatusAssetPath));
	UNTEST_EXPECT_TRUE(StatusAssetPath.Contains(TEXT("EQS_CombatWaiting_Strafe")));

	// Close
	TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
	CloseArgs->SetStringField(TEXT("session_id"), SessionId);

	auto CloseResult = CloseTool.Execute(CloseArgs);
	UNTEST_ASSERT_FALSE(CloseResult.bIsError);

	co_return;
}


#endif // WITH_UNTESTED
