// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// WI-17 regression tests for bp_apply_delta node-reference ergonomics and
// abort transparency:
//   - GUID prefixes of >= 8 hex chars resolve when they match exactly one node.
//   - A prefix matching two nodes is an ambiguity error naming both GUIDs.
//   - An aborted batch attaches a structured report to the error Data
//     (node_specs, id_map, failing_connection_index) and rolls the graph back.

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/IClaireonTool.h"
#include "Tools/ClaireonTool_ApplyBlueprintDelta.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase.h"
#include "ClaireonBlueprintHelpers.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"

#include "ClaireonTestAssetDeletion.h"
// ---------------------------------------------------------------------------
// Test asset path + helpers. Every anon-namespace symbol carries the
// ApplyDeltaRef_ discriminator: anon namespaces are not isolation under unity
// batching, and OpenTestSession/cleanup helpers with other names already exist
// in ClaireonTool_ApplyBlueprintDeltaTests.cpp.
// ---------------------------------------------------------------------------
static const TCHAR* ApplyDeltaRefTestAssetPath = TEXT("/Game/__MCPTests/BP_ApplyDeltaRefTest");

namespace ClaireonApplyDeltaRefTestsInternal
{

// True only when the fixture actually has a .uasset on disk.
//
// This suite's fixture is built by bp_create, whose creation path
// (ClaireonBlueprintHelpers::CreateBlueprint) never saves, and no tool the tests
// invoke (bp_open, bp_apply_delta) saves either -- so the fixture normally lives
// in an in-memory package only. Deleting such a fixture buys nothing, and every
// ObjectTools::ForceDeleteObjects call runs a whole-object-graph referencer scan,
// which is the trigger for the nondeterministic Niagara-serialization crash
// documented in Docs/llm/todo/claireon-untest-harness-reliability.md item 1.
//
// The check is kept rather than dropping the delete outright because
// /Game/__MCPTests is deliberately NOT gitignored: a stale .uasset left by an
// older build or a crashed run must still be cleaned so `git status --porcelain
// -- Content/` stays empty.
bool ApplyDeltaRef_HasFileOnDisk(const FString& AssetOrPackagePath)
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

void ApplyDeltaRef_CleanupTestAsset(const FString& AssetPath)
{
	// In-memory fixture: nothing on disk, nothing to clean, no referencer scan.
	if (!ApplyDeltaRef_HasFileOnDisk(AssetPath))
	{
		return;
	}

	UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *AssetPath);
	if (IsValid(Asset))
	{
		TArray<UObject*> AssetsToDelete;
		AssetsToDelete.Add(Asset);
		ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
	}
}

// Create a scratch Actor BP + open a bp session. Returns the session_id;
// empty string on failure.
FString ApplyDeltaRef_OpenTestSession(const TCHAR* AssetPath)
{
	{
		ClaireonBlueprintGraphTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
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

TSharedPtr<FJsonValue> ApplyDeltaRef_MakeObj(const TSharedPtr<FJsonObject>& Obj)
{
	return MakeShared<FJsonValueObject>(Obj);
}

TSharedPtr<FJsonObject> ApplyDeltaRef_MakeConn(
	const FString& From, const FString& FromPin, const FString& To, const FString& ToPin)
{
	TSharedPtr<FJsonObject> Conn = MakeShared<FJsonObject>();
	Conn->SetStringField(TEXT("from"), From);
	Conn->SetStringField(TEXT("from_pin"), FromPin);
	Conn->SetStringField(TEXT("to"), To);
	Conn->SetStringField(TEXT("to_pin"), ToPin);
	return Conn;
}

// Run one apply_delta creating a Sequence (id 'seq') and a PrintString
// CallFunction (id 'print'). On success fills the two DigitsWithHyphens GUID
// strings from id_map and returns true.
bool ApplyDeltaRef_CreateSeqAndPrint(const FString& SessionId, FString& OutSeqGuid, FString& OutPrintGuid)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	TSharedPtr<FJsonObject> SeqNode = MakeShared<FJsonObject>();
	SeqNode->SetStringField(TEXT("id"), TEXT("seq"));
	SeqNode->SetStringField(TEXT("node_type"), TEXT("Sequence"));

	TSharedPtr<FJsonObject> PrintNode = MakeShared<FJsonObject>();
	PrintNode->SetStringField(TEXT("id"), TEXT("print"));
	PrintNode->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	PrintNode->SetStringField(TEXT("function_name"), TEXT("PrintString"));
	PrintNode->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));

	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(ApplyDeltaRef_MakeObj(SeqNode));
	Nodes.Add(ApplyDeltaRef_MakeObj(PrintNode));
	Args->SetArrayField(TEXT("nodes"), Nodes);

	ClaireonTool_ApplyBlueprintDelta Tool;
	auto Result = Tool.Execute(Args);
	if (Result.bIsError || !Result.Data.IsValid()) return false;

	const TSharedPtr<FJsonObject>* IdMap = nullptr;
	if (!Result.Data->TryGetObjectField(TEXT("id_map"), IdMap) || !IdMap || !(*IdMap).IsValid()) return false;
	if (!(*IdMap)->TryGetStringField(TEXT("seq"), OutSeqGuid) || OutSeqGuid.IsEmpty()) return false;
	if (!(*IdMap)->TryGetStringField(TEXT("print"), OutPrintGuid) || OutPrintGuid.IsEmpty()) return false;
	return true;
}

// Look up the session graph for a session id; nullptr if unavailable.
UEdGraph* ApplyDeltaRef_GetSessionGraph(const FString& SessionId)
{
	FBlueprintEditToolData* ToolData = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
	if (!ToolData) return nullptr;
	return ToolData->Graph.Get();
}

// Find a node in the session graph by a DigitsWithHyphens GUID string.
UEdGraphNode* ApplyDeltaRef_FindNodeByGuidStr(const FString& SessionId, const FString& GuidStr)
{
	FGuid NodeGuid;
	if (!FGuid::Parse(GuidStr, NodeGuid)) return nullptr;
	UEdGraph* Graph = ApplyDeltaRef_GetSessionGraph(SessionId);
	if (!IsValid(Graph)) return nullptr;
	return ClaireonBlueprintHelpers::FindNodeByGuid(Graph, NodeGuid);
}

// True if FromNode.FromPin is linked to ToNode.ToPin.
bool ApplyDeltaRef_PinsLinked(
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

} // namespace ClaireonApplyDeltaRefTestsInternal

using namespace ClaireonApplyDeltaRefTestsInternal;

// ============================================================================
// 1) A unique 8-char GUID prefix in a connection ref resolves to the node.
//    The other endpoint uses the full hyphenated GUID (backward-compat check
//    of the previously-working form in the same call).
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, ApplyDeltaRef, UniqueEightCharGuidPrefix_ResolvesInConnection, UNTEST_TIMEOUTMS(30000))
{
	ApplyDeltaRef_CleanupTestAsset(ApplyDeltaRefTestAssetPath);
	FString SessionId = ApplyDeltaRef_OpenTestSession(ApplyDeltaRefTestAssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	FString SeqGuid, PrintGuid;
	UNTEST_ASSERT_TRUE(ApplyDeltaRef_CreateSeqAndPrint(SessionId, SeqGuid, PrintGuid));

	// DigitsWithHyphens: first 8 chars are the leading hex block (hyphen is at
	// index 8). Random GUIDs sharing an 8-hex prefix is a ~2^-32 event; assert
	// the precondition explicitly so a freak collision reads as a precondition
	// failure, not a product bug.
	const FString SeqPrefix = SeqGuid.Left(8);
	UNTEST_ASSERT_TRUE(SeqPrefix != PrintGuid.Left(8));

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	TArray<TSharedPtr<FJsonValue>> Conns;
	Conns.Add(ApplyDeltaRef_MakeObj(ApplyDeltaRef_MakeConn(
		SeqPrefix, TEXT("then_0"), PrintGuid, TEXT("execute"))));
	Args->SetArrayField(TEXT("connections"), Conns);

	ClaireonTool_ApplyBlueprintDelta Tool;
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	double ConnectionsMade = 0.0;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetNumberField(TEXT("connections_made"), ConnectionsMade));
	UNTEST_EXPECT_EQ(static_cast<int32>(ConnectionsMade), 1);

	// The link must actually exist on the pins, not just be reported.
	UEdGraphNode* SeqNode = ApplyDeltaRef_FindNodeByGuidStr(SessionId, SeqGuid);
	UEdGraphNode* PrintNode = ApplyDeltaRef_FindNodeByGuidStr(SessionId, PrintGuid);
	UNTEST_ASSERT_PTR(SeqNode);
	UNTEST_ASSERT_PTR(PrintNode);
	UNTEST_EXPECT_TRUE(ApplyDeltaRef_PinsLinked(SeqNode, TEXT("then_0"), PrintNode, TEXT("execute")));

	ApplyDeltaRef_CleanupTestAsset(ApplyDeltaRefTestAssetPath);
	co_return;
}

// ============================================================================
// 2) A prefix matching two nodes is an ambiguity error naming both GUIDs.
//    NodeGuid is forced on two nodes so they deterministically share the
//    leading 8 hex chars.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, ApplyDeltaRef, PrefixMatchingTwoNodes_AmbiguityErrorNamesBothGuids, UNTEST_TIMEOUTMS(30000))
{
	ApplyDeltaRef_CleanupTestAsset(ApplyDeltaRefTestAssetPath);
	FString SessionId = ApplyDeltaRef_OpenTestSession(ApplyDeltaRefTestAssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	FString SeqGuid, PrintGuid;
	UNTEST_ASSERT_TRUE(ApplyDeltaRef_CreateSeqAndPrint(SessionId, SeqGuid, PrintGuid));

	UEdGraphNode* SeqNode = ApplyDeltaRef_FindNodeByGuidStr(SessionId, SeqGuid);
	UEdGraphNode* PrintNode = ApplyDeltaRef_FindNodeByGuidStr(SessionId, PrintGuid);
	UNTEST_ASSERT_PTR(SeqNode);
	UNTEST_ASSERT_PTR(PrintNode);

	// Force a shared 8-hex-char prefix (0xABCDEF01) on both nodes.
	const FGuid ForcedGuidA(0xABCDEF01u, 0x00000001u, 0x00000002u, 0x00000003u);
	const FGuid ForcedGuidB(0xABCDEF01u, 0x00000004u, 0x00000005u, 0x00000006u);
	SeqNode->NodeGuid = ForcedGuidA;
	PrintNode->NodeGuid = ForcedGuidB;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	TArray<TSharedPtr<FJsonValue>> Conns;
	Conns.Add(ApplyDeltaRef_MakeObj(ApplyDeltaRef_MakeConn(
		TEXT("ABCDEF01"), TEXT("then_0"),
		ForcedGuidB.ToString(EGuidFormats::DigitsWithHyphens), TEXT("execute"))));
	Args->SetArrayField(TEXT("connections"), Conns);

	ClaireonTool_ApplyBlueprintDelta Tool;
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);

	// The ambiguity error must name BOTH candidate GUIDs so the caller can
	// disambiguate without another round-trip.
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("ambiguous")));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(ForcedGuidA.ToString(EGuidFormats::DigitsWithHyphens)));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(ForcedGuidB.ToString(EGuidFormats::DigitsWithHyphens)));

	// The abort report identifies the failing connection.
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());
	double FailingIdx = -1.0;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetNumberField(TEXT("failing_connection_index"), FailingIdx));
	UNTEST_EXPECT_EQ(static_cast<int32>(FailingIdx), 0);

	ApplyDeltaRef_CleanupTestAsset(ApplyDeltaRefTestAssetPath);
	co_return;
}

// ============================================================================
// 3) A batch with one bad connection aborts with a structured report: error
//    Data carries id_map (the rolled-back local-id -> GUID mappings), the
//    per-spec node_specs statuses, and the failing connection index; the graph
//    is unchanged (transaction rolled back).
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, ApplyDeltaRef, BadConnection_AbortReportAndRollback, UNTEST_TIMEOUTMS(30000))
{
	ApplyDeltaRef_CleanupTestAsset(ApplyDeltaRefTestAssetPath);
	FString SessionId = ApplyDeltaRef_OpenTestSession(ApplyDeltaRefTestAssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UEdGraph* Graph = ApplyDeltaRef_GetSessionGraph(SessionId);
	UNTEST_ASSERT_PTR(Graph);
	const int32 PreNodeCount = Graph->Nodes.Num();

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);

	TSharedPtr<FJsonObject> SeqNode = MakeShared<FJsonObject>();
	SeqNode->SetStringField(TEXT("id"), TEXT("seq"));
	SeqNode->SetStringField(TEXT("node_type"), TEXT("Sequence"));

	TSharedPtr<FJsonObject> PrintNode = MakeShared<FJsonObject>();
	PrintNode->SetStringField(TEXT("id"), TEXT("print"));
	PrintNode->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	PrintNode->SetStringField(TEXT("function_name"), TEXT("PrintString"));
	PrintNode->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));

	TArray<TSharedPtr<FJsonValue>> Nodes;
	Nodes.Add(ApplyDeltaRef_MakeObj(SeqNode));
	Nodes.Add(ApplyDeltaRef_MakeObj(PrintNode));
	Args->SetArrayField(TEXT("nodes"), Nodes);

	// connections[0] is valid; connections[1] names a pin that does not exist.
	TArray<TSharedPtr<FJsonValue>> Conns;
	Conns.Add(ApplyDeltaRef_MakeObj(ApplyDeltaRef_MakeConn(
		TEXT("seq"), TEXT("then_0"), TEXT("print"), TEXT("execute"))));
	Conns.Add(ApplyDeltaRef_MakeObj(ApplyDeltaRef_MakeConn(
		TEXT("seq"), TEXT("then_1"), TEXT("print"), TEXT("bogus_pin_xyz"))));
	Args->SetArrayField(TEXT("connections"), Conns);

	ClaireonTool_ApplyBlueprintDelta Tool;
	auto Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	// id_map must still be present so the caller can see both node specs
	// resolved and were created (then rolled back).
	const TSharedPtr<FJsonObject>* IdMap = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("id_map"), IdMap));
	UNTEST_ASSERT_TRUE(IdMap && (*IdMap).IsValid());
	FString SeqGuid, PrintGuid;
	UNTEST_EXPECT_TRUE((*IdMap)->TryGetStringField(TEXT("seq"), SeqGuid));
	UNTEST_EXPECT_FALSE(SeqGuid.IsEmpty());
	UNTEST_EXPECT_TRUE((*IdMap)->TryGetStringField(TEXT("print"), PrintGuid));
	UNTEST_EXPECT_FALSE(PrintGuid.IsEmpty());

	// The failing connection index is the caller's array position.
	double FailingIdx = -1.0;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetNumberField(TEXT("failing_connection_index"), FailingIdx));
	UNTEST_EXPECT_EQ(static_cast<int32>(FailingIdx), 1);

	// Per-spec statuses: both created specs are reported as rolled back.
	const TArray<TSharedPtr<FJsonValue>>* NodeSpecs = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("node_specs"), NodeSpecs));
	UNTEST_ASSERT_TRUE(NodeSpecs && NodeSpecs->Num() == 2);
	int32 RolledBackCount = 0;
	for (const TSharedPtr<FJsonValue>& SpecVal : *NodeSpecs)
	{
		const TSharedPtr<FJsonObject>* SpecObj = nullptr;
		if (SpecVal.IsValid() && SpecVal->TryGetObject(SpecObj) && SpecObj && (*SpecObj).IsValid())
		{
			FString Status;
			if ((*SpecObj)->TryGetStringField(TEXT("status"), Status)
				&& Status == TEXT("created_rolled_back"))
			{
				++RolledBackCount;
			}
		}
	}
	UNTEST_EXPECT_EQ(RolledBackCount, 2);

	// Graph unchanged: the created nodes were removed on abort.
	UNTEST_EXPECT_EQ(Graph->Nodes.Num(), PreNodeCount);

	// And the rolled-back GUIDs must NOT resolve to live nodes.
	UNTEST_EXPECT_TRUE(ApplyDeltaRef_FindNodeByGuidStr(SessionId, SeqGuid) == nullptr);
	UNTEST_EXPECT_TRUE(ApplyDeltaRef_FindNodeByGuidStr(SessionId, PrintGuid) == nullptr);

	ApplyDeltaRef_CleanupTestAsset(ApplyDeltaRefTestAssetPath);
	co_return;
}

#endif // WITH_UNTESTED
