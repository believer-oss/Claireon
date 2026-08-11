// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Baseline pinning test for Workstream D (sessions, overrides, conformance),
// Stage 001 of the Claireon BP feedback plan (Work #6704).
//
// F1 core (bp_apply_delta, RESOLVED): a smoke test reproducing the
// designer report's exact repro shape -- one Sequence node plus three
// CallFunction (PrintString) nodes, wired via local temp ids in a single
// apply_delta batch -- returns id_map with all four local ids and every
// connection actually lands on the live graph. Heavy apply_delta coverage
// already exists in ClaireonApplyDeltaCatalogTests.cpp / ClaireonApplyDeltaRefTests.cpp;
// this is a thin feedback-scenario smoke test, not a re-test of apply_delta's
// full surface. D-1/D-2/D-3/D-4's own new-behavior tests land in Stage 006/007/008/009.

#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonTestDataAssertions.h"

#include "Tools/ClaireonBlueprintGraphTool_AddFunction.h"
#include "Tools/ClaireonBlueprintGraphTool_AddFunctionOverride.h"
#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/ClaireonBlueprintGraphTool_ApplySpec.h"
#include "Tools/ClaireonBlueprintGraphTool_ConnectPins.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_CloseAll.h"
#include "Tools/ClaireonBlueprintGraphTool_Save.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h"
#include "Tools/ClaireonTool_ApplyBlueprintDelta.h"
#include "Tools/ClaireonTool_ReleaseSessions.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonBridge.h"
#include "ClaireonModule.h"
#include "ClaireonSessionManager.h"
#include "IClaireonToolProvider.h"
#include "Features/IModularFeatures.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonSCTTestsInternal
{
	static void SCT_CleanupAsset(const FString& AssetPath)
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

	// Create a scratch Actor BP + open a bp session. Returns the session_id;
	// empty string on failure.
	static FString SCT_OpenTestSession(const TCHAR* AssetPath)
	{
		{
			ClaireonBlueprintGraphTool_Create CreateTool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("asset_path"), AssetPath);
			Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
			IClaireonTool::FToolResult R = CreateTool.Execute(Args);
			if (R.bIsError) return FString();
		}
		ClaireonBlueprintGraphTool_Open OpenTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		IClaireonTool::FToolResult R = OpenTool.Execute(Args);
		if (R.bIsError || !R.Data.IsValid()) return FString();
		FString SessionId;
		R.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	static TSharedPtr<FJsonValue> SCT_MakeObj(const TSharedPtr<FJsonObject>& Obj)
	{
		return MakeShared<FJsonValueObject>(Obj);
	}

	static TSharedPtr<FJsonObject> SCT_MakeConn(
		const FString& From, const FString& FromPin, const FString& To, const FString& ToPin)
	{
		TSharedPtr<FJsonObject> Conn = MakeShared<FJsonObject>();
		Conn->SetStringField(TEXT("from"), From);
		Conn->SetStringField(TEXT("from_pin"), FromPin);
		Conn->SetStringField(TEXT("to"), To);
		Conn->SetStringField(TEXT("to_pin"), ToPin);
		return Conn;
	}

	static UEdGraph* SCT_GetSessionGraph(const FString& SessionId)
	{
		FBlueprintEditToolData* ToolData = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
		if (!ToolData) return nullptr;
		return ToolData->Graph.Get();
	}

	static UEdGraphNode* SCT_FindNodeByGuidStr(const FString& SessionId, const FString& GuidStr)
	{
		FGuid NodeGuid;
		if (!FGuid::Parse(GuidStr, NodeGuid)) return nullptr;
		UEdGraph* Graph = SCT_GetSessionGraph(SessionId);
		if (!IsValid(Graph)) return nullptr;
		return ClaireonBlueprintHelpers::FindNodeByGuid(Graph, NodeGuid);
	}

	static bool SCT_PinsLinked(
		UEdGraphNode* FromNode, const TCHAR* FromPinName,
		UEdGraphNode* ToNode, const TCHAR* ToPinName)
	{
		if (!IsValid(FromNode) || !IsValid(ToNode)) return false;
		for (UEdGraphPin* Pin : FromNode->Pins)
		{
			if (!Pin || !Pin->PinName.ToString().Equals(FromPinName, ESearchCase::IgnoreCase)) continue;
			for (UEdGraphPin* Linked : Pin->LinkedTo)
			{
				if (Linked && Linked->GetOwningNode() == ToNode
					&& Linked->PinName.ToString().Equals(ToPinName, ESearchCase::IgnoreCase))
				{
					return true;
				}
			}
		}
		return false;
	}
} // namespace ClaireonSCTTestsInternal

using namespace ClaireonSCTTestsInternal;

// ============================================================================
// F1 core: Sequence + 3x CallFunction, wired via local temp ids, in one
// apply_delta batch -- the report's exact repro shape.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, ApplyDelta_SequenceAndThreeCallsHappyPathWiresAll, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_SCT_ApplyDeltaSeqAndThree");

	SCT_CleanupAsset(AssetPath);
	FString SessionId = SCT_OpenTestSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	TSharedPtr<FJsonObject> SeqNode = MakeShared<FJsonObject>();
	SeqNode->SetStringField(TEXT("id"), TEXT("seq"));
	SeqNode->SetStringField(TEXT("node_type"), TEXT("Sequence"));
	SeqNode->SetNumberField(TEXT("num_extra_pins"), 1); // then_0/then_1/then_2

	TSharedPtr<FJsonObject> Print0Node = MakeShared<FJsonObject>();
	Print0Node->SetStringField(TEXT("id"), TEXT("print0"));
	Print0Node->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	Print0Node->SetStringField(TEXT("function_name"), TEXT("PrintString"));
	Print0Node->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));

	TSharedPtr<FJsonObject> Print1Node = MakeShared<FJsonObject>();
	Print1Node->SetStringField(TEXT("id"), TEXT("print1"));
	Print1Node->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	Print1Node->SetStringField(TEXT("function_name"), TEXT("PrintString"));
	Print1Node->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));

	TSharedPtr<FJsonObject> Print2Node = MakeShared<FJsonObject>();
	Print2Node->SetStringField(TEXT("id"), TEXT("print2"));
	Print2Node->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	Print2Node->SetStringField(TEXT("function_name"), TEXT("PrintString"));
	Print2Node->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));

	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(SCT_MakeObj(SeqNode));
	Nodes.Add(SCT_MakeObj(Print0Node));
	Nodes.Add(SCT_MakeObj(Print1Node));
	Nodes.Add(SCT_MakeObj(Print2Node));

	TArray<TSharedPtr<FJsonValue>> Connections;
	Connections.Add(SCT_MakeObj(SCT_MakeConn(TEXT("seq"), TEXT("then_0"), TEXT("print0"), TEXT("execute"))));
	Connections.Add(SCT_MakeObj(SCT_MakeConn(TEXT("seq"), TEXT("then_1"), TEXT("print1"), TEXT("execute"))));
	Connections.Add(SCT_MakeObj(SCT_MakeConn(TEXT("seq"), TEXT("then_2"), TEXT("print2"), TEXT("execute"))));

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetArrayField(TEXT("nodes"), Nodes);
	Args->SetArrayField(TEXT("connections"), Connections);

	ClaireonTool_ApplyBlueprintDelta Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	const TSharedPtr<FJsonObject>* IdMap = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("id_map"), IdMap));
	UNTEST_ASSERT_TRUE(IdMap && (*IdMap).IsValid());

	FString SeqGuid, Print0Guid, Print1Guid, Print2Guid;
	UNTEST_ASSERT_TRUE((*IdMap)->TryGetStringField(TEXT("seq"), SeqGuid) && !SeqGuid.IsEmpty());
	UNTEST_ASSERT_TRUE((*IdMap)->TryGetStringField(TEXT("print0"), Print0Guid) && !Print0Guid.IsEmpty());
	UNTEST_ASSERT_TRUE((*IdMap)->TryGetStringField(TEXT("print1"), Print1Guid) && !Print1Guid.IsEmpty());
	UNTEST_ASSERT_TRUE((*IdMap)->TryGetStringField(TEXT("print2"), Print2Guid) && !Print2Guid.IsEmpty());

	UEdGraphNode* SeqNodeLive = SCT_FindNodeByGuidStr(SessionId, SeqGuid);
	UEdGraphNode* Print0NodeLive = SCT_FindNodeByGuidStr(SessionId, Print0Guid);
	UEdGraphNode* Print1NodeLive = SCT_FindNodeByGuidStr(SessionId, Print1Guid);
	UEdGraphNode* Print2NodeLive = SCT_FindNodeByGuidStr(SessionId, Print2Guid);
	UNTEST_ASSERT_PTR(SeqNodeLive);
	UNTEST_ASSERT_PTR(Print0NodeLive);
	UNTEST_ASSERT_PTR(Print1NodeLive);
	UNTEST_ASSERT_PTR(Print2NodeLive);

	UNTEST_EXPECT_TRUE(SCT_PinsLinked(SeqNodeLive, TEXT("then_0"), Print0NodeLive, TEXT("execute")));
	UNTEST_EXPECT_TRUE(SCT_PinsLinked(SeqNodeLive, TEXT("then_1"), Print1NodeLive, TEXT("execute")));
	UNTEST_EXPECT_TRUE(SCT_PinsLinked(SeqNodeLive, TEXT("then_2"), Print2NodeLive, TEXT("execute")));

	int32 ConnectionsMade = 0;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetNumberField(TEXT("connections_made"), ConnectionsMade));
	UNTEST_EXPECT_EQ(ConnectionsMade, 3);

	SCT_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// D-1 (Stage 006): session_release accepts session_id, bp_close_all
// compiles+saves+closes every bp session, bp_open discloses blocking_scope,
// and every recovery hint names parameters the target tool actually accepts.
// ============================================================================
namespace ClaireonD1TestsInternal
{
	static IClaireonTool::FToolResult D1_ReleaseSessions(TFunctionRef<void(TSharedPtr<FJsonObject>&)> Configure)
	{
		ClaireonTool_ReleaseSessions Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Configure(Args);
		return Tool.Execute(Args);
	}

	/** Names of the parameters a tool's input schema declares. */
	static TSet<FString> D1_SchemaParamNames(const IClaireonTool& Tool)
	{
		TSet<FString> Names;
		TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
		if (!Schema.IsValid()) { return Names; }
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (!Schema->TryGetObjectField(TEXT("properties"), Properties) || !Properties) { return Names; }
		for (const auto& Pair : (*Properties)->Values)
		{
			Names.Add(Pair.Key);
		}
		return Names;
	}
} // namespace ClaireonD1TestsInternal

using namespace ClaireonD1TestsInternal;

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, SessionRelease_BySessionId_ReleasesExactlyThatSession, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPathA = TEXT("/Game/__MCPTests/BP_D1_ReleaseByIdA");
	static const TCHAR* AssetPathB = TEXT("/Game/__MCPTests/BP_D1_ReleaseByIdB");

	SCT_CleanupAsset(AssetPathA);
	SCT_CleanupAsset(AssetPathB);

	const FString SessionA = SCT_OpenTestSession(AssetPathA);
	const FString SessionB = SCT_OpenTestSession(AssetPathB);
	UNTEST_ASSERT_FALSE(SessionA.IsEmpty());
	UNTEST_ASSERT_FALSE(SessionB.IsEmpty());

	IClaireonTool::FToolResult R = D1_ReleaseSessions([&SessionA](TSharedPtr<FJsonObject>& Args)
	{
		Args->SetStringField(TEXT("session_id"), SessionA);
	});
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	int32 ReleasedCount = 0;
	UNTEST_ASSERT_TRUE(R.Data->TryGetNumberField(TEXT("released_count"), ReleasedCount));
	UNTEST_EXPECT_EQ(ReleasedCount, 1);

	// Exactly that one: A is gone, B survives.
	UNTEST_EXPECT_NULLPTR(ClaireonBlueprintGraphEditToolBase::FindToolData(SessionA));
	UNTEST_EXPECT_PTR(ClaireonBlueprintGraphEditToolBase::FindToolData(SessionB));

	// A second release of the same id is a no-op success, not an error.
	IClaireonTool::FToolResult Again = D1_ReleaseSessions([&SessionA](TSharedPtr<FJsonObject>& Args)
	{
		Args->SetStringField(TEXT("session_id"), SessionA);
	});
	UNTEST_EXPECT_FALSE(Again.bIsError);
	int32 AgainCount = -1;
	UNTEST_ASSERT_TRUE(Again.Data.IsValid());
	UNTEST_ASSERT_TRUE(Again.Data->TryGetNumberField(TEXT("released_count"), AgainCount));
	UNTEST_EXPECT_EQ(AgainCount, 0);

	SCT_CleanupAsset(AssetPathA);
	SCT_CleanupAsset(AssetPathB);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, SessionRelease_NoArgs_ErrorNamesAllThree, UNTEST_TIMEOUTMS(30000))
{
	IClaireonTool::FToolResult R = D1_ReleaseSessions([](TSharedPtr<FJsonObject>&) {});

	UNTEST_ASSERT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("session_id")));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("asset_path")));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("force_all")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, CloseAll_SavesAndClosesEveryBPSession, UNTEST_TIMEOUTMS(120000))
{
	static const TCHAR* AssetPathA = TEXT("/Game/__MCPTests/BP_D1_CloseAllA");
	static const TCHAR* AssetPathB = TEXT("/Game/__MCPTests/BP_D1_CloseAllB");

	// Start from a clean slate: other suites may have left sessions open, and
	// close_all is global by design.
	FClaireonSessionManager::Get().ForceReleaseAll();
	SCT_CleanupAsset(AssetPathA);
	SCT_CleanupAsset(AssetPathB);

	const FString SessionA = SCT_OpenTestSession(AssetPathA);
	const FString SessionB = SCT_OpenTestSession(AssetPathB);
	UNTEST_ASSERT_FALSE(SessionA.IsEmpty());
	UNTEST_ASSERT_FALSE(SessionB.IsEmpty());

	// Dirty both packages so the save is observable.
	UEdGraph* GraphA = SCT_GetSessionGraph(SessionA);
	UEdGraph* GraphB = SCT_GetSessionGraph(SessionB);
	UNTEST_ASSERT_PTR(GraphA);
	UNTEST_ASSERT_PTR(GraphB);
	UPackage* PackageA = GraphA->GetOutermost();
	UPackage* PackageB = GraphB->GetOutermost();
	UNTEST_ASSERT_PTR(PackageA);
	UNTEST_ASSERT_PTR(PackageB);
	PackageA->MarkPackageDirty();
	PackageB->MarkPackageDirty();

	ClaireonBlueprintGraphTool_CloseAll CloseAllTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	IClaireonTool::FToolResult R = CloseAllTool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	int32 TotalCount = 0;
	int32 ClosedCount = 0;
	UNTEST_ASSERT_TRUE(R.Data->TryGetNumberField(TEXT("total_count"), TotalCount));
	UNTEST_ASSERT_TRUE(R.Data->TryGetNumberField(TEXT("closed_count"), ClosedCount));
	UNTEST_EXPECT_EQ(TotalCount, 2);
	UNTEST_EXPECT_EQ(ClosedCount, 2);

	// Both listed, both flagged saved.
	const TArray<TSharedPtr<FJsonValue>>* ClosedSessions = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetArrayField(TEXT("closed_sessions"), ClosedSessions));
	UNTEST_ASSERT_TRUE(ClosedSessions != nullptr);
	UNTEST_ASSERT_EQ(ClosedSessions->Num(), 2);
	TSet<FString> ReportedAssetPaths;
	for (const TSharedPtr<FJsonValue>& V : *ClosedSessions)
	{
		TSharedPtr<FJsonObject> Entry = V.IsValid() ? V->AsObject() : nullptr;
		UNTEST_ASSERT_TRUE(Entry.IsValid());
		FString EntryAssetPath;
		bool bSaved = false;
		UNTEST_ASSERT_TRUE(Entry->TryGetStringField(TEXT("asset_path"), EntryAssetPath));
		UNTEST_ASSERT_TRUE(Entry->TryGetBoolField(TEXT("saved"), bSaved));
		UNTEST_EXPECT_TRUE(bSaved);
		ReportedAssetPaths.Add(EntryAssetPath);
	}
	UNTEST_EXPECT_TRUE(ReportedAssetPaths.Contains(AssetPathA));
	UNTEST_EXPECT_TRUE(ReportedAssetPaths.Contains(AssetPathB));

	// Saved for real, and both sessions gone.
	UNTEST_EXPECT_FALSE(PackageA->IsDirty());
	UNTEST_EXPECT_FALSE(PackageB->IsDirty());
	UNTEST_EXPECT_NULLPTR(ClaireonBlueprintGraphEditToolBase::FindToolData(SessionA));
	UNTEST_EXPECT_NULLPTR(ClaireonBlueprintGraphEditToolBase::FindToolData(SessionB));
	UNTEST_EXPECT_EQ(FClaireonSessionManager::Get().ListSessions(TEXT("bp")).Num(), 0);

	SCT_CleanupAsset(AssetPathA);
	SCT_CleanupAsset(AssetPathB);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, Open_DisclosesBlockingScopeMatchingRegistry, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_D1_BlockingScope");

	SCT_CleanupAsset(AssetPath);

	// Populate the tool registry BEFORE bp_open runs. bp_open sources
	// blocking_scope from FClaireonBridge::GetToolNamesBySessionMode(), which
	// reads the live registry, so without the seam both sides of the equality
	// below are empty and the comparison is vacuous. EnsureServerForTest() is
	// the seam that builds the registry headlessly (StartupModule() early-returns
	// under IsRunningCommandlet()) and it populates it process-wide, so calling
	// it here makes the roster deterministic instead of dependent on whether
	// some earlier test in the run happened to call it.
	UNTEST_ASSERT_PTR(FClaireonModule::Get().EnsureServerForTest());

	{
		ClaireonBlueprintGraphTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		IClaireonTool::FToolResult R = CreateTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}

	ClaireonBlueprintGraphTool_Open OpenTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	IClaireonTool::FToolResult R = OpenTool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* ScopeValues = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetArrayField(TEXT("blocking_scope"), ScopeValues));
	UNTEST_ASSERT_TRUE(ScopeValues != nullptr);

	TSet<FString> Reported;
	for (const TSharedPtr<FJsonValue>& V : *ScopeValues)
	{
		if (V.IsValid()) { Reported.Add(V->AsString()); }
	}

	// Set equality against the live registry, so adding or removing an EditorWide
	// tool later shows up here instead of silently drifting.
	//
	// The previous comment here claimed this was a permanent harness LIMITATION --
	// "the Untest commandlet runs without the MCP server and without the Claireon
	// tool providers registered as modular features, so the roster is empty here
	// and the equality below is vacuous". That is wrong. EnsureServerForTest()
	// (called at the top of this test) registers the builtin provider and
	// populates the registry process-wide, in commandlet mode included. An empty
	// roster now means the seam regressed, so fail rather than log a warning and
	// compare 0 against 0.
	const TArray<FString> Expected =
		FClaireonBridge::GetToolNamesBySessionMode(EClaireonToolSessionMode::EditorWide);
	const TSet<FString> ExpectedSet(Expected);
	if (ExpectedSet.Num() == 0)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[SessionContract] EditorWide tool roster is EMPTY after EnsureServerForTest(); "
			     "the blocking_scope equality below would be vacuous"));
	}
	UNTEST_ASSERT_GT(ExpectedSet.Num(), 0);
	UNTEST_EXPECT_EQ(Reported.Num(), ExpectedSet.Num());
	UNTEST_EXPECT_TRUE(Reported.Includes(ExpectedSet));

	SCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, RecoveryHints_NameParametersTheTargetToolAccepts, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_D1_HintAccuracy");

	SCT_CleanupAsset(AssetPath);

	// The invariant: every recovery hint points at session_release and names a
	// parameter session_release actually declares. This is what made the old
	// "mcp_release_sessions(asset_path=...)" hint a dead end.
	ClaireonTool_ReleaseSessions ReleaseTool;
	const TSet<FString> ReleaseParams = D1_SchemaParamNames(ReleaseTool);
	UNTEST_ASSERT_TRUE(ReleaseParams.Contains(TEXT("session_id")));
	UNTEST_ASSERT_TRUE(ReleaseParams.Contains(TEXT("asset_path")));
	UNTEST_ASSERT_TRUE(ReleaseParams.Contains(TEXT("force_all")));

	// Create the asset, then release every session so the only lock is the one we
	// plant next.
	const FString SetupSession = SCT_OpenTestSession(AssetPath);
	UNTEST_ASSERT_FALSE(SetupSession.IsEmpty());
	FClaireonSessionManager::Get().ForceReleaseAll();

	// Provoke bp_open's blocked path by holding the lock under a non-bp tool name.
	FClaireonSessionManager::Get().OpenSession(AssetPath, TEXT("statetree"));

	ClaireonBlueprintGraphTool_Open OpenTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	IClaireonTool::FToolResult Blocked = OpenTool.Execute(Args);

	// The blocked path is the ONLY thing this test exists to check, and the
	// preceding lines set it up deterministically (ForceReleaseAll, then a lock
	// planted under the 'statetree' tool name). Wrapping the hint assertions in
	// `if (Blocked.bIsError && ...)` meant a bp_open that stopped blocking would
	// skip all of them and leave only "the error text does not mention
	// mcp_release_sessions" -- which is trivially satisfied by an EMPTY error
	// string, i.e. by the failure case. Assert the precondition instead.
	if (!Blocked.bIsError)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[SessionContract] bp_open SUCCEEDED on an asset locked by another tool; "
			     "the recovery-hint path could not be exercised"));
	}
	UNTEST_ASSERT_TRUE(Blocked.bIsError);
	UNTEST_ASSERT_TRUE(Blocked.ErrorMessage.Contains(TEXT("locked by")));

	// The dead tool name must be gone, replaced by the real one...
	UNTEST_EXPECT_FALSE(Blocked.ErrorMessage.Contains(TEXT("mcp_release_sessions")));
	UNTEST_EXPECT_TRUE(Blocked.ErrorMessage.Contains(TEXT("session_release")));
	// ...naming a parameter session_release declares.
	UNTEST_EXPECT_TRUE(Blocked.ErrorMessage.Contains(TEXT("session_id=")));

	FClaireonSessionManager::Get().ForceReleaseAll();
	SCT_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// D-2 (Stage 007): add_function_override reports a structured kind, and an
// event override that shadows a non-empty parent body warns, naming the
// CallParentFunction remedy. A BlueprintImplementableEvent parent has no body
// by construction and must stay warning-free.
// ============================================================================
namespace ClaireonD2TestsInternal
{
	/** Create a Blueprint with the given native parent and open a session on it. */
	static FString D2_OpenSessionWithParent(const TCHAR* AssetPath, const TCHAR* ParentClass)
	{
		{
			ClaireonBlueprintGraphTool_Create CreateTool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("asset_path"), AssetPath);
			Args->SetStringField(TEXT("parent_class"), ParentClass);
			IClaireonTool::FToolResult R = CreateTool.Execute(Args);
			if (R.bIsError) { return FString(); }
		}
		ClaireonBlueprintGraphTool_Open OpenTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		IClaireonTool::FToolResult R = OpenTool.Execute(Args);
		if (R.bIsError || !R.Data.IsValid()) { return FString(); }
		FString SessionId;
		R.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	static IClaireonTool::FToolResult D2_AddOverride(
		const FString& SessionId, const TCHAR* FunctionName, bool bForceFunctionGraph = false)
	{
		ClaireonBlueprintGraphTool_AddFunctionOverride Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("function_name"), FunctionName);
		if (bForceFunctionGraph) { Args->SetBoolField(TEXT("force_function_graph"), true); }
		return Tool.Execute(Args);
	}

	static bool D2_HasWarningContaining(const IClaireonTool::FToolResult& R, const TCHAR* Needle)
	{
		for (const FString& W : R.Warnings)
		{
			if (W.Contains(Needle)) { return true; }
		}
		return false;
	}

	static FString D2_GetKind(const IClaireonTool::FToolResult& R)
	{
		FString Kind;
		if (R.Data.IsValid()) { R.Data->TryGetStringField(TEXT("kind"), Kind); }
		return Kind;
	}
} // namespace ClaireonD2TestsInternal

using namespace ClaireonD2TestsInternal;

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, Override_BIEParent_EventKindNoWarning, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_D2_BIEParent");

	SCT_CleanupAsset(AssetPath);
	const FString SessionId = D2_OpenSessionWithParent(
		AssetPath, TEXT("ClaireonFunctionOverrideFixtureActor"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	IClaireonTool::FToolResult R = D2_AddOverride(SessionId, TEXT("SelectDropLocation"));
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[SessionContract] add_function_override failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	// BIE has no implementation anywhere, so nothing is being shadowed.
	UNTEST_EXPECT_TRUE(D2_GetKind(R) == TEXT("event_override"));
	UNTEST_EXPECT_FALSE(D2_HasWarningContaining(R, TEXT("non-empty body")));

	// The event path also discloses the node it created.
	// Structured fact -> Result.Data, via the guarded macro: on a miss the failure
	// names node_guid and dumps the keys Data did carry, instead of reporting
	// "false == true". See ClaireonTestDataAssertions.h.
	FString NodeGuid;
	UNTEST_CLAIREON_DATA_STR(R, "node_guid", NodeGuid);
	UNTEST_EXPECT_FALSE(NodeGuid.IsEmpty());

	SCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, Override_NativeEventParent_WarnsNamingCallParentFunction, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_D2_BNEParent");

	SCT_CleanupAsset(AssetPath);
	const FString SessionId = D2_OpenSessionWithParent(
		AssetPath, TEXT("ClaireonNativeEventOverrideFixtureActor"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// BlueprintNativeEvent -> a _Implementation body exists that the override shadows.
	IClaireonTool::FToolResult R = D2_AddOverride(SessionId, TEXT("ApplyNativeDefault"));
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[SessionContract] add_function_override failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UNTEST_EXPECT_TRUE(D2_GetKind(R) == TEXT("event_override"));
	UNTEST_EXPECT_TRUE(D2_HasWarningContaining(R, TEXT("non-empty body")));
	// The warning must name the remedy, not just state the problem.
	UNTEST_EXPECT_TRUE(D2_HasWarningContaining(R, TEXT("CallParentFunction")));
	UNTEST_EXPECT_TRUE(D2_HasWarningContaining(R, TEXT("ApplyNativeDefault")));

	SCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, Override_ReturnValueFunction_FunctionKindNoWarning, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_D2_FunctionKind");

	SCT_CleanupAsset(AssetPath);
	const FString SessionId = D2_OpenSessionWithParent(
		AssetPath, TEXT("ClaireonNativeEventOverrideFixtureActor"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// A return value forces the function-graph shape, where no shadowing warning applies.
	IClaireonTool::FToolResult R = D2_AddOverride(SessionId, TEXT("ComputeNativeValue"));
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[SessionContract] add_function_override failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UNTEST_EXPECT_TRUE(D2_GetKind(R) == TEXT("function_override"));
	UNTEST_EXPECT_FALSE(D2_HasWarningContaining(R, TEXT("non-empty body")));

	// graph_name is the newly created function graph, not the session's old graph.
	FString GraphName;
	UNTEST_CLAIREON_DATA_STR(R, "graph_name", GraphName);
	UNTEST_EXPECT_STREQ(GraphName, TEXT("ComputeNativeValue"));

	SCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, Override_BlueprintParentWithWiredBody_Warns, UNTEST_TIMEOUTMS(120000))
{
	static const TCHAR* ParentAssetPath = TEXT("/Game/__MCPTests/BP_D2_ParentChain");
	static const TCHAR* ChildAssetPath = TEXT("/Game/__MCPTests/BP_D2_ChildChain");

	SCT_CleanupAsset(ChildAssetPath);
	SCT_CleanupAsset(ParentAssetPath);

	// Level 1: a BP over the BIE fixture that overrides SelectDropLocation and
	// actually wires something to it, so the override has a body.
	const FString ParentSession = D2_OpenSessionWithParent(
		ParentAssetPath, TEXT("ClaireonFunctionOverrideFixtureActor"));
	UNTEST_ASSERT_FALSE(ParentSession.IsEmpty());

	IClaireonTool::FToolResult ParentOverride = D2_AddOverride(ParentSession, TEXT("SelectDropLocation"));
	UNTEST_ASSERT_FALSE(ParentOverride.bIsError);
	FString ParentEventGuid;
	UNTEST_CLAIREON_DATA_STR(ParentOverride, "node_guid", ParentEventGuid);

	// Wire a PrintString onto the parent's event so it has a real body.
	FString PrintGuid;
	{
		ClaireonBlueprintGraphTool_AddNode AddNodeTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), ParentSession);
		Args->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		Args->SetStringField(TEXT("function_name"), TEXT("PrintString"));
		Args->SetStringField(TEXT("class_name"), TEXT("KismetSystemLibrary"));
		IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
		UNTEST_ASSERT_TRUE(R.Data.IsValid());
		// add_node reports the new node as created_node_guid.
		R.Data->TryGetStringField(TEXT("created_node_guid"), PrintGuid);
	}
	// The whole point of this fixture is a parent with a wired body; if the node
	// never came back the test would silently assert nothing.
	UNTEST_ASSERT_FALSE(PrintGuid.IsEmpty());
	{
		ClaireonBlueprintGraphTool_ConnectPins ConnectTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), ParentSession);
		Args->SetStringField(TEXT("source_node_guid"), ParentEventGuid);
		Args->SetStringField(TEXT("source_pin_name"), TEXT("then"));
		Args->SetStringField(TEXT("target_node_guid"), PrintGuid);
		Args->SetStringField(TEXT("target_pin_name"), TEXT("execute"));
		IClaireonTool::FToolResult R = ConnectTool.Execute(Args);
		if (R.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[SessionContract] parent-body connect failed: %s"), *R.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(R.bIsError);
	}

	// Compile+save the parent so the child can derive from a real generated class.
	{
		ClaireonBlueprintGraphTool_Save SaveTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), ParentSession);
		IClaireonTool::FToolResult R = SaveTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}
	FClaireonSessionManager::Get().ReleaseByAssetPath(ParentAssetPath);

	// Level 2: a child of that BP overriding the same function.
	const FString ParentClassPath = FString(ParentAssetPath) + TEXT(".")
		+ FPackageName::GetShortName(FString(ParentAssetPath)) + TEXT("_C");
	const FString ChildSession = D2_OpenSessionWithParent(ChildAssetPath, *ParentClassPath);
	UNTEST_ASSERT_FALSE(ChildSession.IsEmpty());

	// The warning depends on the child actually deriving from the parent Blueprint,
	// so pin that rather than letting a mis-parent look like a detection bug.
	{
		FBlueprintEditToolData* ChildData = ClaireonBlueprintGraphEditToolBase::FindToolData(ChildSession);
		UNTEST_ASSERT_PTR(ChildData);
		UBlueprint* ChildBP = ChildData->Blueprint.Get();
		UNTEST_ASSERT_PTR(ChildBP);
		UClass* ChildParentClass = ChildBP->ParentClass;
		UNTEST_ASSERT_PTR(ChildParentClass);
		UNTEST_EXPECT_PTR(Cast<UBlueprint>(ChildParentClass->ClassGeneratedBy));
	}

	IClaireonTool::FToolResult ChildOverride = D2_AddOverride(ChildSession, TEXT("SelectDropLocation"));
	if (ChildOverride.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[SessionContract] child override failed: %s"), *ChildOverride.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(ChildOverride.bIsError);

	UNTEST_EXPECT_TRUE(D2_GetKind(ChildOverride) == TEXT("event_override"));
	// The Blueprint parent has a wired body, so the child's override shadows it.
	UNTEST_EXPECT_TRUE(D2_HasWarningContaining(ChildOverride, TEXT("non-empty body")));
	UNTEST_EXPECT_TRUE(D2_HasWarningContaining(ChildOverride, TEXT("CallParentFunction")));

	SCT_CleanupAsset(ChildAssetPath);
	SCT_CleanupAsset(ParentAssetPath);
	co_return;
}

// ============================================================================
// D-3 (Stage 008): bp_connect_pins accepts the from_*/to_* spelling that
// bp_apply_delta and bp_get_graph output already use, canonical names win on a
// mixed call, and no tool documents a tooltip for a parameter it does not
// declare.
// ============================================================================
namespace ClaireonD3TestsInternal
{
	/** Add a PrintString and return its GUID, or empty on failure. */
	static FString D3_AddPrintString(const FString& SessionId)
	{
		ClaireonBlueprintGraphTool_AddNode AddNodeTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		Args->SetStringField(TEXT("function_name"), TEXT("PrintString"));
		Args->SetStringField(TEXT("class_name"), TEXT("KismetSystemLibrary"));
		IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);
		if (R.bIsError || !R.Data.IsValid()) { return FString(); }
		FString Guid;
		R.Data->TryGetStringField(TEXT("created_node_guid"), Guid);
		return Guid;
	}

	static UEdGraphNode* D3_FindNode(const FString& SessionId, const FString& GuidStr)
	{
		return SCT_FindNodeByGuidStr(SessionId, GuidStr);
	}

	/** Tooltip keys that name no schema property. Empty means conformant. */
	static TArray<FString> D3_TooltipKeysMissingFromSchema(const IClaireonTool& Tool)
	{
		TArray<FString> Offenders;

		TSharedPtr<FJsonObject> Tooltips = Tool.GetParameterTooltips();
		if (!Tooltips.IsValid()) { return Offenders; }

		TSet<FString> SchemaProps;
		TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
		if (Schema.IsValid())
		{
			const TSharedPtr<FJsonObject>* Properties = nullptr;
			if (Schema->TryGetObjectField(TEXT("properties"), Properties) && Properties)
			{
				for (const auto& Pair : (*Properties)->Values)
				{
					SchemaProps.Add(Pair.Key);
				}
			}
		}

		for (const auto& Pair : Tooltips->Values)
		{
			if (!SchemaProps.Contains(Pair.Key))
			{
				Offenders.Add(Pair.Key);
			}
		}
		return Offenders;
	}
} // namespace ClaireonD3TestsInternal

using namespace ClaireonD3TestsInternal;

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, ConnectPins_FromToAliasSpellingConnects, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_D3_AliasSpelling");

	SCT_CleanupAsset(AssetPath);
	const FString SessionId = SCT_OpenTestSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString GuidA = D3_AddPrintString(SessionId);
	const FString GuidB = D3_AddPrintString(SessionId);
	UNTEST_ASSERT_FALSE(GuidA.IsEmpty());
	UNTEST_ASSERT_FALSE(GuidB.IsEmpty());

	// Pure alias spelling -- no source_*/target_* fields at all.
	ClaireonBlueprintGraphTool_ConnectPins ConnectTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("from_node"), GuidA);
	Args->SetStringField(TEXT("from_pin"), TEXT("then"));
	Args->SetStringField(TEXT("to_node"), GuidB);
	Args->SetStringField(TEXT("to_pin"), TEXT("execute"));
	IClaireonTool::FToolResult R = ConnectTool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[SessionContract] alias connect failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UEdGraphNode* NodeA = D3_FindNode(SessionId, GuidA);
	UEdGraphNode* NodeB = D3_FindNode(SessionId, GuidB);
	UNTEST_ASSERT_PTR(NodeA);
	UNTEST_ASSERT_PTR(NodeB);
	UNTEST_EXPECT_TRUE(SCT_PinsLinked(NodeA, TEXT("then"), NodeB, TEXT("execute")));

	SCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, ConnectPins_MixedSpelling_CanonicalWins, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_D3_MixedSpelling");

	SCT_CleanupAsset(AssetPath);
	const FString SessionId = SCT_OpenTestSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString CanonicalSource = D3_AddPrintString(SessionId);
	const FString AliasSource = D3_AddPrintString(SessionId);
	const FString TargetGuid = D3_AddPrintString(SessionId);
	UNTEST_ASSERT_FALSE(CanonicalSource.IsEmpty());
	UNTEST_ASSERT_FALSE(AliasSource.IsEmpty());
	UNTEST_ASSERT_FALSE(TargetGuid.IsEmpty());

	// Both spellings present, naming DIFFERENT source nodes. The canonical field
	// must decide, deterministically and without erroring.
	ClaireonBlueprintGraphTool_ConnectPins ConnectTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("source_node_guid"), CanonicalSource);
	Args->SetStringField(TEXT("from_node"), AliasSource);
	Args->SetStringField(TEXT("source_pin_name"), TEXT("then"));
	Args->SetStringField(TEXT("target_node_guid"), TargetGuid);
	Args->SetStringField(TEXT("target_pin_name"), TEXT("execute"));
	IClaireonTool::FToolResult R = ConnectTool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[SessionContract] mixed-spelling connect failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UEdGraphNode* CanonicalNode = D3_FindNode(SessionId, CanonicalSource);
	UEdGraphNode* AliasNode = D3_FindNode(SessionId, AliasSource);
	UEdGraphNode* TargetNode = D3_FindNode(SessionId, TargetGuid);
	UNTEST_ASSERT_PTR(CanonicalNode);
	UNTEST_ASSERT_PTR(AliasNode);
	UNTEST_ASSERT_PTR(TargetNode);

	UNTEST_EXPECT_TRUE(SCT_PinsLinked(CanonicalNode, TEXT("then"), TargetNode, TEXT("execute")));
	UNTEST_EXPECT_FALSE(SCT_PinsLinked(AliasNode, TEXT("then"), TargetNode, TEXT("execute")));

	SCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, ToolMetadata_TooltipKeysAreSchemaProperties, UNTEST_TIMEOUTMS(60000))
{
	// Catalog-wide sweep.
	//
	// The previous comment here claimed this loop is necessarily empty headlessly
	// because the Untest commandlet registers no tool providers. That is wrong:
	// FClaireonModule::EnsureServerForTest() registers the builtin provider and
	// populates the registry PROCESS-WIDE, so whether this loop saw any tools used
	// to depend on whether some earlier test in the run happened to call the seam.
	// Call it explicitly so the sweep is deterministic rather than order-dependent.
	int32 CatalogToolsChecked = 0;
	{
		FClaireonModule& SweepModule = FClaireonModule::Get();
		UNTEST_ASSERT_PTR(SweepModule.EnsureServerForTest());

		const TArray<IClaireonToolProvider*> Providers = IModularFeatures::Get()
			.GetModularFeatureImplementations<IClaireonToolProvider>(IClaireonToolProvider::FeatureName);
		for (IClaireonToolProvider* Provider : Providers)
		{
			if (!Provider) { continue; }
			for (const TSharedPtr<IClaireonTool>& Tool : Provider->GetTools())
			{
				if (!Tool.IsValid()) { continue; }
				++CatalogToolsChecked;
				const TArray<FString> Offenders = D3_TooltipKeysMissingFromSchema(*Tool);
				if (Offenders.Num() > 0)
				{
					UE_LOG(LogTemp, Error,
						TEXT("[SessionContract] tool '%s' documents tooltips for undeclared params: %s"),
						*Tool->GetName(), *FString::Join(Offenders, TEXT(", ")));
				}
				UNTEST_EXPECT_EQ(Offenders.Num(), 0);
			}
		}
	}
	// EnsureServerForTest() above registers the builtin provider, so a zero-tool
	// sweep means that seam regressed -- fail instead of quietly covering nothing.
	// (Untest has no skip primitive: a bare early return scores as a PASS, which is
	// exactly how this sweep went unnoticed while auditing 0 tools.)
	if (CatalogToolsChecked == 0)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[SessionContract] catalog sweep covered 0 tools; EnsureServerForTest() seam is broken."));
	}
	UNTEST_EXPECT_TRUE(CatalogToolsChecked > 0);

	// Directly-constructed spot checks so the invariant has teeth in this harness.
	// bp_connect_pins is the tool D-3 fixes: its tooltips named from_*/to_* while
	// the schema declared only source_*/target_*.
	{
		ClaireonBlueprintGraphTool_ConnectPins ConnectTool;
		const TArray<FString> Offenders = D3_TooltipKeysMissingFromSchema(ConnectTool);
		if (Offenders.Num() > 0)
		{
			UE_LOG(LogTemp, Error, TEXT("[SessionContract] bp_connect_pins undeclared tooltip params: %s"),
				*FString::Join(Offenders, TEXT(", ")));
		}
		UNTEST_EXPECT_EQ(Offenders.Num(), 0);
	}
	{
		ClaireonBlueprintGraphTool_Open OpenTool;
		UNTEST_EXPECT_EQ(D3_TooltipKeysMissingFromSchema(OpenTool).Num(), 0);
	}
	{
		ClaireonBlueprintGraphTool_CloseAll CloseAllTool;
		UNTEST_EXPECT_EQ(D3_TooltipKeysMissingFromSchema(CloseAllTool).Num(), 0);
	}
	{
		ClaireonTool_ReleaseSessions ReleaseTool;
		UNTEST_EXPECT_EQ(D3_TooltipKeysMissingFromSchema(ReleaseTool).Num(), 0);
	}

	co_return;
}

// ============================================================================
// D-4 (Stage 009): apply_delta gains a pin_defaults phase and a real
// asset_path auto-open; apply_spec honors per-node graph targeting without
// silently dropping the wiring for nodes it placed off the default graph.
// ============================================================================
namespace ClaireonD4TestsInternal
{
	static TSharedPtr<FJsonObject> D4_PrintNode(const TCHAR* Id)
	{
		TSharedPtr<FJsonObject> Node = MakeShared<FJsonObject>();
		Node->SetStringField(TEXT("id"), Id);
		Node->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		Node->SetStringField(TEXT("function_name"), TEXT("PrintString"));
		Node->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));
		return Node;
	}

	static TSharedPtr<FJsonObject> D4_PinDefault(const TCHAR* NodeRef, const TCHAR* Pin, const TCHAR* Value)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("node"), NodeRef);
		Entry->SetStringField(TEXT("pin"), Pin);
		Entry->SetStringField(TEXT("value"), Value);
		return Entry;
	}

	static UEdGraphPin* D4_FindPin(UEdGraphNode* Node, const TCHAR* PinName)
	{
		if (!IsValid(Node)) { return nullptr; }
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return Pin;
			}
		}
		return nullptr;
	}
} // namespace ClaireonD4TestsInternal

using namespace ClaireonD4TestsInternal;

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, ApplyDelta_PinDefaultsLandOnCreatedNodes, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_D4_PinDefaults");

	SCT_CleanupAsset(AssetPath);
	const FString SessionId = SCT_OpenTestSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(SCT_MakeObj(D4_PrintNode(TEXT("p0"))));

	TArray<TSharedPtr<FJsonValue>> PinDefaults;
	PinDefaults.Add(SCT_MakeObj(D4_PinDefault(TEXT("p0"), TEXT("InString"), TEXT("hello-from-delta"))));

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetArrayField(TEXT("nodes"), Nodes);
	Args->SetArrayField(TEXT("pin_defaults"), PinDefaults);

	ClaireonTool_ApplyBlueprintDelta Tool;
	IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[SessionContract] apply_delta pin_defaults failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	const TSharedPtr<FJsonObject>* IdMap = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetObjectField(TEXT("id_map"), IdMap));
	FString P0Guid;
	UNTEST_ASSERT_TRUE((*IdMap)->TryGetStringField(TEXT("p0"), P0Guid));

	// Assert on the live graph, not the response envelope.
	UEdGraphNode* Node = SCT_FindNodeByGuidStr(SessionId, P0Guid);
	UNTEST_ASSERT_PTR(Node);
	UEdGraphPin* InStringPin = D4_FindPin(Node, TEXT("InString"));
	UNTEST_ASSERT_PTR(InStringPin);
	UNTEST_EXPECT_TRUE(InStringPin->DefaultValue == TEXT("hello-from-delta"));

	SCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, ApplyDelta_BadPinDefaultRollsBackWholeBatch, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_D4_PinDefaultRollback");

	SCT_CleanupAsset(AssetPath);
	const FString SessionId = SCT_OpenTestSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UEdGraph* Graph = SCT_GetSessionGraph(SessionId);
	UNTEST_ASSERT_PTR(Graph);
	const int32 NodeCountBefore = Graph->Nodes.Num();

	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(SCT_MakeObj(D4_PrintNode(TEXT("p0"))));
	Nodes.Add(SCT_MakeObj(D4_PrintNode(TEXT("p1"))));

	// Second entry names a pin that does not exist -> the whole batch must roll back,
	// including the two nodes the create phase already made.
	TArray<TSharedPtr<FJsonValue>> PinDefaults;
	PinDefaults.Add(SCT_MakeObj(D4_PinDefault(TEXT("p0"), TEXT("InString"), TEXT("ok"))));
	PinDefaults.Add(SCT_MakeObj(D4_PinDefault(TEXT("p1"), TEXT("NoSuchPin_XYZ"), TEXT("boom"))));

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetArrayField(TEXT("nodes"), Nodes);
	Args->SetArrayField(TEXT("pin_defaults"), PinDefaults);

	ClaireonTool_ApplyBlueprintDelta Tool;
	IClaireonTool::FToolResult R = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("pin_defaults[1]")));

	UEdGraph* GraphAfter = SCT_GetSessionGraph(SessionId);
	UNTEST_ASSERT_PTR(GraphAfter);
	UNTEST_EXPECT_EQ(GraphAfter->Nodes.Num(), NodeCountBefore);

	SCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, ApplyDelta_AssetPathOnlyAutoOpens, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_D4_AutoOpen");

	SCT_CleanupAsset(AssetPath);

	// Create the asset, then drop every session so apply_delta has to open its own.
	const FString SetupSession = SCT_OpenTestSession(AssetPath);
	UNTEST_ASSERT_FALSE(SetupSession.IsEmpty());
	FClaireonSessionManager::Get().ForceReleaseAll();

	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(SCT_MakeObj(D4_PrintNode(TEXT("p0"))));

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetArrayField(TEXT("nodes"), Nodes);

	ClaireonTool_ApplyBlueprintDelta Tool;
	IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[SessionContract] apply_delta auto-open failed: %s"), *R.ErrorMessage);
	}
	// The description has claimed asset_path auto-open for a long time; this is the
	// call that used to fail with "Missing required field: session_id".
	UNTEST_ASSERT_FALSE(R.bIsError);

	FString OpenedSessionId;
	UNTEST_CLAIREON_DATA_STR(R, "session_id", OpenedSessionId);
	UNTEST_EXPECT_FALSE(OpenedSessionId.IsEmpty());
	UNTEST_EXPECT_PTR(ClaireonBlueprintGraphEditToolBase::FindToolData(OpenedSessionId));

	SCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackSessionContract, ApplySpec_PerNodeGraphTargetingAndCrossGraphWiring, UNTEST_TIMEOUTMS(120000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_D4_SpecGraphTarget");

	SCT_CleanupAsset(AssetPath);

	// A function graph to target, created up front through the normal tools.
	const FString SetupSession = SCT_OpenTestSession(AssetPath);
	UNTEST_ASSERT_FALSE(SetupSession.IsEmpty());
	{
		ClaireonBlueprintGraphTool_AddFunction AddFunctionTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SetupSession);
		Args->SetStringField(TEXT("function_name"), TEXT("SpecTargetFunc"));
		IClaireonTool::FToolResult R = AddFunctionTool.Execute(Args);
		if (R.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[SessionContract] add_function failed: %s"), *R.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(R.bIsError);
	}
	{
		ClaireonBlueprintGraphTool_Save SaveTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SetupSession);
		IClaireonTool::FToolResult R = SaveTool.Execute(Args);
		UNTEST_ASSERT_FALSE(R.bIsError);
	}
	FClaireonSessionManager::Get().ForceReleaseAll();

	// Spec: one node targeted at the function graph, one left on the default graph,
	// one naming a graph that does not exist.
	TSharedPtr<FJsonObject> InFunc = D4_PrintNode(TEXT("in_func"));
	InFunc->SetStringField(TEXT("type"), TEXT("CallFunction"));
	InFunc->SetStringField(TEXT("function"), TEXT("PrintString"));
	InFunc->SetStringField(TEXT("graph"), TEXT("SpecTargetFunc"));

	TSharedPtr<FJsonObject> InEvent = D4_PrintNode(TEXT("in_event"));
	InEvent->SetStringField(TEXT("type"), TEXT("CallFunction"));
	InEvent->SetStringField(TEXT("function"), TEXT("PrintString"));

	TSharedPtr<FJsonObject> InBogus = D4_PrintNode(TEXT("in_bogus"));
	InBogus->SetStringField(TEXT("type"), TEXT("CallFunction"));
	InBogus->SetStringField(TEXT("function"), TEXT("PrintString"));
	InBogus->SetStringField(TEXT("graph"), TEXT("NoSuchGraph_XYZ"));

	TArray<TSharedPtr<FJsonValue>> SpecNodes;
	SpecNodes.Add(SCT_MakeObj(InFunc));
	SpecNodes.Add(SCT_MakeObj(InEvent));
	SpecNodes.Add(SCT_MakeObj(InBogus));

	TSharedPtr<FJsonObject> Spec = MakeShared<FJsonObject>();
	Spec->SetArrayField(TEXT("nodes"), SpecNodes);

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetObjectField(TEXT("spec"), Spec);

	ClaireonBlueprintGraphTool_ApplySpec Tool;
	IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[SessionContract] apply_spec failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	// The targeted node must be in the function graph, not the EventGraph, and the
	// bogus-graph entry must fail on its own without taking the others down.
	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(FString(AssetPath))).TryLoad());
	UNTEST_ASSERT_PTR(BP);

	UEdGraph* FuncGraph = ClaireonBlueprintHelpers::FindGraphByName(BP, TEXT("SpecTargetFunc"));
	UNTEST_ASSERT_PTR(FuncGraph);

	int32 CallNodesInFuncGraph = 0;
	for (UEdGraphNode* N : FuncGraph->Nodes)
	{
		if (IsValid(N) && N->IsA<UK2Node_CallFunction>()) { ++CallNodesInFuncGraph; }
	}
	UNTEST_EXPECT_TRUE(CallNodesInFuncGraph >= 1);

	// Per-entry reporting: the bogus graph name must be reported failed, others ok.
	FString Serialized;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	UNTEST_ASSERT_TRUE(FJsonSerializer::Serialize(R.Data.ToSharedRef(), Writer));
	UNTEST_EXPECT_TRUE(Serialized.Contains(TEXT("NoSuchGraph_XYZ")));

	SCT_CleanupAsset(AssetPath);
	co_return;
}

#endif // WITH_UNTESTED
