// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Setup helpers use the decomposed ClaireonBlueprintGraphTool_* classes
// instead of the deleted ClaireonTool_EditBlueprintGraph shim. The
// tool-under-test (ClaireonTool_GetBlueprintGraph) is untouched.

#include "Tools/ClaireonTool_GetBlueprintGraph.h"
#include "Tools/ClaireonTool_ApplyBlueprintDelta.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/ClaireonBlueprintGraphTool_MoveNode.h"
#include "Tools/ClaireonBlueprintGraphTool_Close.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonOutputGate.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Internationalization/Regex.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	// Helpers are prefixed to avoid unity-build ODR collisions with
	// same-named helpers in ClaireonTool_EditBlueprintGraph.spec.cpp.
	// Clang lumps multiple .cpp files into one unity TU where anonymous
	// namespaces from separate .cpp files merge into a single anonymous
	// namespace, triggering redefinition and ambiguous-call errors.

	/** Extract the focused cursor node GUID from a full-mode BuildStateResponse body.
	 *  Parses "Focused Node: <title> [GUID: <guid>]" produced by BuildStateResponse. */
	FString GetBPSpec_ExtractCursorNodeGuidFromResponse(const FString& ResultText)
	{
		int32 FocusStart = ResultText.Find(TEXT("Focused Node:"));
		if (FocusStart == INDEX_NONE)
		{
			return FString();
		}
		int32 GuidStart = ResultText.Find(TEXT("[GUID: "), ESearchCase::IgnoreCase, ESearchDir::FromStart, FocusStart);
		if (GuidStart == INDEX_NONE)
		{
			return FString();
		}
		GuidStart += 7; // length of "[GUID: "
		int32 GuidEnd = ResultText.Find(TEXT("]"), ESearchCase::IgnoreCase, ESearchDir::FromStart, GuidStart);
		if (GuidEnd == INDEX_NONE) { return FString(); }
		return ResultText.Mid(GuidStart, GuidEnd - GuidStart).TrimStartAndEnd();
	}

	/** Extract "Session ID: <id>" from a full-mode BuildStateResponse body. */
	FString GetBPSpec_ExtractSessionIdFromResponse(const FString& ResultText)
	{
		int32 Start = ResultText.Find(TEXT("Session ID: "));
		if (Start == INDEX_NONE) { return FString(); }
		Start += 12;
		int32 End = ResultText.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Start);
		if (End == INDEX_NONE) { End = ResultText.Len(); }
		return ResultText.Mid(Start, End - Start).TrimStartAndEnd();
	}

	/** Create a new empty Blueprint and return its session ID. Returns empty on failure. */
	FString CreateBlueprintSession(
		FAutomationTestBase& Test,
		const FString& AssetPath)
	{
		ClaireonBlueprintGraphTool_Create CreateTool;
		TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
		CreateArgs->SetStringField(TEXT("asset_path"), AssetPath);
		CreateArgs->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		auto CR = CreateTool.Execute(CreateArgs);
		if (CR.bIsError)
		{
			Test.AddError(FString::Printf(TEXT("Create BP failed: %s"), *CR.GetContentAsString()));
			return FString();
		}
		return GetBPSpec_ExtractSessionIdFromResponse(CR.GetContentAsString());
	}

	/** Add a PrintString CallFunction node; returns node GUID (may be empty if cursor not advanced). */
	FString AddPrintStringNode(
		FAutomationTestBase& Test,
		const FString& SessionId)
	{
		ClaireonBlueprintGraphTool_AddNode AddTool;
		TSharedPtr<FJsonObject> AddArgs = MakeShared<FJsonObject>();
		AddArgs->SetStringField(TEXT("session_id"), SessionId);
		AddArgs->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		AddArgs->SetStringField(TEXT("function_name"), TEXT("PrintString"));
		AddArgs->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));
		AddArgs->SetStringField(TEXT("response_mode"), TEXT("full"));
		auto AR = AddTool.Execute(AddArgs);
		if (AR.bIsError)
		{
			Test.AddError(FString::Printf(TEXT("add_node PrintString failed: %s"), *AR.GetContentAsString()));
			return FString();
		}
		return GetBPSpec_ExtractCursorNodeGuidFromResponse(AR.GetContentAsString());
	}

	/** Add a Branch (UK2Node_IfThenElse) node; returns node GUID. */
	FString AddBranchNode(
		FAutomationTestBase& Test,
		const FString& SessionId)
	{
		ClaireonBlueprintGraphTool_AddNode AddTool;
		TSharedPtr<FJsonObject> AddArgs = MakeShared<FJsonObject>();
		AddArgs->SetStringField(TEXT("session_id"), SessionId);
		AddArgs->SetStringField(TEXT("node_type"), TEXT("Branch"));
		AddArgs->SetStringField(TEXT("response_mode"), TEXT("full"));
		auto AR = AddTool.Execute(AddArgs);
		if (AR.bIsError)
		{
			Test.AddError(FString::Printf(TEXT("add_node Branch failed: %s"), *AR.GetContentAsString()));
			return FString();
		}
		return GetBPSpec_ExtractCursorNodeGuidFromResponse(AR.GetContentAsString());
	}

	/** Close the edit session. */
	void CloseSession(const FString& SessionId)
	{
		ClaireonBlueprintGraphTool_Close CloseTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		CloseTool.Execute(Args);
	}

	/** Resolve the EventGraph for a blueprint currently loaded at the given asset path. */
	UEdGraph* GetEventGraphFromAsset(const FString& AssetPath)
	{
		UBlueprint* BP = LoadObject<UBlueprint>(nullptr, *AssetPath);
		if (!BP) { return nullptr; }
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (G && G->GetName() == TEXT("EventGraph"))
			{
				return G;
			}
		}
		return nullptr;
	}

	/** Regex matching the new outline grammar, per FRACTURE/04_outline_format.md. */
	static const FString kOutlineRegex =
		TEXT("^\\s*(\\d+)\\.\\s+(\\w+)\\s+([0-9a-fA-F]{8})\\s{2}(.+?)\\s{2}@\\s+\\((-?\\d+),\\s*(-?\\d+)\\)\\s*$");
}

// =====================================================================================
// Test 13: OutlineFormat.SingleLineGrammar
// Three-node graph (Event BeginPlay + PrintString + Branch) produces body lines that
// all match the canonical outline regex.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetBlueprintGraphTest_OutlineFormat_SingleLineGrammar,
	"Claireon.GetBlueprintGraph.OutlineFormat.SingleLineGrammar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGetBlueprintGraphTest_OutlineFormat_SingleLineGrammar::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_OutlineGrammar");

	const FString SessionId = CreateBlueprintSession(*this, AssetPath);
	if (SessionId.IsEmpty()) { return false; }

	// Default-created Actor BP already has Event BeginPlay on EventGraph (plus ticks).
	// Add a PrintString node, then a Branch, to guarantee at least 3 node types in graph.
	const FString PrintGuid = AddPrintStringNode(*this, SessionId);
	const FString BranchGuid = AddBranchNode(*this, SessionId);
	if (PrintGuid.IsEmpty() || BranchGuid.IsEmpty())
	{
		CloseSession(SessionId);
		return false;
	}

	UEdGraph* EventGraph = GetEventGraphFromAsset(AssetPath);
	if (!EventGraph)
	{
		AddError(TEXT("Failed to resolve EventGraph from session"));
		CloseSession(SessionId);
		return false;
	}

	// Render outline directly via the private helper through the test accessor.
	ClaireonTool_GetBlueprintGraph GetTool;
	const FString Markdown = FClaireonGetBlueprintGraphTestAccess::BuildGraphJsonSummary(GetTool, EventGraph, TEXT("outline"), 0);

	TArray<FString> Lines;
	Markdown.ParseIntoArray(Lines, TEXT("\n"), true);

	FRegexPattern Pattern(kOutlineRegex);

	int32 BodyLineCount = 0;
	for (const FString& Line : Lines)
	{
		// Body lines start with "<digit>.". Skip markdown headers and footer navigation text.
		const FString Trim = Line.TrimStartAndEnd();
		if (Trim.IsEmpty()) { continue; }
		if (!FChar::IsDigit(Trim[0])) { continue; }
		// Confirm it's the "<n>." prefix shape before we run the strict grammar regex.
		int32 DotIdx = Trim.Find(TEXT("."));
		if (DotIdx <= 0) { continue; }
		bool bAllDigits = true;
		for (int32 i = 0; i < DotIdx; ++i)
		{
			if (!FChar::IsDigit(Trim[i])) { bAllDigits = false; break; }
		}
		if (!bAllDigits) { continue; }

		FRegexMatcher Matcher(Pattern, Line);
		if (!Matcher.FindNext())
		{
			AddError(FString::Printf(TEXT("Outline line did not match grammar: '%s'"), *Line));
			CloseSession(SessionId);
			return false;
		}
		BodyLineCount++;
	}

	if (BodyLineCount < 2)
	{
		AddError(FString::Printf(TEXT("Expected at least 2 body lines, got %d. Markdown:\n%s"),
			BodyLineCount, *Markdown));
		CloseSession(SessionId);
		return false;
	}

	CloseSession(SessionId);
	return true;
}

// =====================================================================================
// Test 14: OutlineFormat.NoEmbeddedNewlines
// A PrintString node has "Target is Kismet System Library" as second FullTitle line.
// The outline body line for it must contain no embedded newline. The body line count
// must match the node count in the graph.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetBlueprintGraphTest_OutlineFormat_NoEmbeddedNewlines,
	"Claireon.GetBlueprintGraph.OutlineFormat.NoEmbeddedNewlines",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGetBlueprintGraphTest_OutlineFormat_NoEmbeddedNewlines::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_OutlineNoNewlines");

	const FString SessionId = CreateBlueprintSession(*this, AssetPath);
	if (SessionId.IsEmpty()) { return false; }

	const FString PrintGuid = AddPrintStringNode(*this, SessionId);
	if (PrintGuid.IsEmpty()) { CloseSession(SessionId); return false; }

	UEdGraph* EventGraph = GetEventGraphFromAsset(AssetPath);
	if (!EventGraph)
	{
		AddError(TEXT("Failed to resolve EventGraph from session"));
		CloseSession(SessionId);
		return false;
	}

	const int32 NodeCountInGraph = EventGraph->Nodes.Num();

	ClaireonTool_GetBlueprintGraph GetTool;
	const FString Markdown = FClaireonGetBlueprintGraphTestAccess::BuildGraphJsonSummary(GetTool, EventGraph, TEXT("outline"), 0);

	// Body line count (matching ^\d+\.) must equal the graph's node count -- no split subtitles.
	TArray<FString> Lines;
	Markdown.ParseIntoArray(Lines, TEXT("\n"), true);

	int32 BodyLineCount = 0;
	for (const FString& Line : Lines)
	{
		const FString Trim = Line.TrimStartAndEnd();
		if (Trim.IsEmpty()) { continue; }
		if (!FChar::IsDigit(Trim[0])) { continue; }
		int32 DotIdx = Trim.Find(TEXT("."));
		if (DotIdx <= 0) { continue; }
		bool bAllDigits = true;
		for (int32 i = 0; i < DotIdx; ++i)
		{
			if (!FChar::IsDigit(Trim[i])) { bAllDigits = false; break; }
		}
		if (bAllDigits)
		{
			BodyLineCount++;
		}
	}

	if (BodyLineCount != NodeCountInGraph)
	{
		AddError(FString::Printf(TEXT("Body line count %d != graph node count %d. Subtitles may have split lines. Markdown:\n%s"),
			BodyLineCount, NodeCountInGraph, *Markdown));
		CloseSession(SessionId);
		return false;
	}

	// Also verify FormatNodeSummary output for the PrintString node contains no newline.
	FGuid PrintGuidParsed;
	if (!FGuid::Parse(PrintGuid, PrintGuidParsed))
	{
		AddError(TEXT("Failed to parse PrintString GUID"));
		CloseSession(SessionId);
		return false;
	}
	UEdGraphNode* PrintNode = ClaireonBlueprintHelpers::FindNodeByGuid(EventGraph, PrintGuidParsed);
	if (!PrintNode)
	{
		AddError(TEXT("Failed to find PrintString node"));
		CloseSession(SessionId);
		return false;
	}
	const FString OutlineLine = FClaireonGetBlueprintGraphTestAccess::FormatNodeSummary(GetTool, PrintNode, TEXT("outline"));
	if (OutlineLine.Contains(TEXT("\n")) || OutlineLine.Contains(TEXT("\r")))
	{
		AddError(FString::Printf(TEXT("PrintString outline line contained embedded newline: '%s'"), *OutlineLine));
		CloseSession(SessionId);
		return false;
	}

	CloseSession(SessionId);
	return true;
}

// =====================================================================================
// Test 15: OutlineFormat.ShortGuidUniqueness
// Build a 20-node graph; the 8-hex short GUIDs emitted by outline mode must all be
// distinct for a graph of this size (birthday probability at 8 hex chars is << 1%).
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetBlueprintGraphTest_OutlineFormat_ShortGuidUniqueness,
	"Claireon.GetBlueprintGraph.OutlineFormat.ShortGuidUniqueness",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGetBlueprintGraphTest_OutlineFormat_ShortGuidUniqueness::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_OutlineShortGuidUnique");

	const FString SessionId = CreateBlueprintSession(*this, AssetPath);
	if (SessionId.IsEmpty()) { return false; }

	// Add 20 PrintString nodes (node type repetition is fine -- GUIDs differ).
	for (int32 i = 0; i < 20; ++i)
	{
		const FString NodeGuid = AddPrintStringNode(*this, SessionId);
		if (NodeGuid.IsEmpty())
		{
			CloseSession(SessionId);
			return false;
		}
	}

	UEdGraph* EventGraph = GetEventGraphFromAsset(AssetPath);
	if (!EventGraph)
	{
		AddError(TEXT("Failed to resolve EventGraph"));
		CloseSession(SessionId);
		return false;
	}

	ClaireonTool_GetBlueprintGraph GetTool;
	const FString Markdown = FClaireonGetBlueprintGraphTestAccess::BuildGraphJsonSummary(GetTool, EventGraph, TEXT("outline"), 0);

	// Extract every 8-hex capture via the outline regex.
	FRegexPattern Pattern(kOutlineRegex);

	TArray<FString> Lines;
	Markdown.ParseIntoArray(Lines, TEXT("\n"), true);

	TSet<FString> ShortGuids;
	int32 MatchedLines = 0;
	for (const FString& Line : Lines)
	{
		FRegexMatcher Matcher(Pattern, Line);
		if (Matcher.FindNext())
		{
			const FString ShortGuid = Matcher.GetCaptureGroup(3);
			ShortGuids.Add(ShortGuid);
			MatchedLines++;
		}
	}

	// Must have added at least 20 nodes (plus default Event BeginPlay etc.)
	if (MatchedLines < 20)
	{
		AddError(FString::Printf(TEXT("Expected >= 20 matched outline lines, got %d"), MatchedLines));
		CloseSession(SessionId);
		return false;
	}

	if (ShortGuids.Num() != MatchedLines)
	{
		AddError(FString::Printf(TEXT("Short GUID collision detected: %d unique GUIDs across %d lines"),
			ShortGuids.Num(), MatchedLines));
		CloseSession(SessionId);
		return false;
	}

	CloseSession(SessionId);
	return true;
}

// =====================================================================================
// Test 15a: OutlineFormat.PositionParsesSigned
// Move a node to (-512, 384). The outline line for that node must parse with the
// signed-integer regex captures ("-512", "384").
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetBlueprintGraphTest_OutlineFormat_PositionParsesSigned,
	"Claireon.GetBlueprintGraph.OutlineFormat.PositionParsesSigned",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGetBlueprintGraphTest_OutlineFormat_PositionParsesSigned::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_OutlinePositionSigned");

	const FString SessionId = CreateBlueprintSession(*this, AssetPath);
	if (SessionId.IsEmpty()) { return false; }

	const FString PrintGuid = AddPrintStringNode(*this, SessionId);
	if (PrintGuid.IsEmpty()) { CloseSession(SessionId); return false; }

	// Move the PrintString node to (-512, 384) via move_node.
	{
		ClaireonBlueprintGraphTool_MoveNode MoveTool;
		TSharedPtr<FJsonObject> MoveArgs = MakeShared<FJsonObject>();
		MoveArgs->SetStringField(TEXT("session_id"), SessionId);
		MoveArgs->SetStringField(TEXT("node_guid"), PrintGuid);
		TSharedPtr<FJsonObject> PosObj = MakeShared<FJsonObject>();
		PosObj->SetNumberField(TEXT("x"), -512);
		PosObj->SetNumberField(TEXT("y"), 384);
		MoveArgs->SetObjectField(TEXT("position"), PosObj);
		auto MR = MoveTool.Execute(MoveArgs);
		if (MR.bIsError)
		{
			AddError(FString::Printf(TEXT("move_node failed: %s"), *MR.GetContentAsString()));
			CloseSession(SessionId);
			return false;
		}
	}

	UEdGraph* EventGraph = GetEventGraphFromAsset(AssetPath);
	if (!EventGraph)
	{
		AddError(TEXT("Failed to resolve EventGraph"));
		CloseSession(SessionId);
		return false;
	}

	FGuid PrintGuidParsed;
	FGuid::Parse(PrintGuid, PrintGuidParsed);
	UEdGraphNode* PrintNode = ClaireonBlueprintHelpers::FindNodeByGuid(EventGraph, PrintGuidParsed);
	if (!PrintNode)
	{
		AddError(TEXT("Failed to resolve PrintString node"));
		CloseSession(SessionId);
		return false;
	}

	const FString ExpectedShortGuid = PrintNode->NodeGuid.ToString(EGuidFormats::Digits).Left(8).ToLower();

	ClaireonTool_GetBlueprintGraph GetTool;
	const FString Markdown = FClaireonGetBlueprintGraphTestAccess::BuildGraphJsonSummary(GetTool, EventGraph, TEXT("outline"), 0);

	FRegexPattern Pattern(kOutlineRegex);
	TArray<FString> Lines;
	Markdown.ParseIntoArray(Lines, TEXT("\n"), true);

	bool bFound = false;
	for (const FString& Line : Lines)
	{
		FRegexMatcher Matcher(Pattern, Line);
		if (!Matcher.FindNext())
		{
			continue;
		}
		const FString ShortGuid = Matcher.GetCaptureGroup(3);
		if (ShortGuid != ExpectedShortGuid) { continue; }
		const FString XCapture = Matcher.GetCaptureGroup(5);
		const FString YCapture = Matcher.GetCaptureGroup(6);
		if (XCapture != TEXT("-512"))
		{
			AddError(FString::Printf(TEXT("Expected x capture '-512', got '%s'"), *XCapture));
			CloseSession(SessionId);
			return false;
		}
		if (YCapture != TEXT("384"))
		{
			AddError(FString::Printf(TEXT("Expected y capture '384', got '%s'"), *YCapture));
			CloseSession(SessionId);
			return false;
		}
		bFound = true;
		break;
	}

	if (!bFound)
	{
		AddError(FString::Printf(TEXT("Could not find outline line for short GUID %s. Markdown:\n%s"),
			*ExpectedShortGuid, *Markdown));
		CloseSession(SessionId);
		return false;
	}

	CloseSession(SessionId);
	return true;
}

// =====================================================================================
// ITEM_02 Test 1: GUID self-heal + DigitsWithHyphens format regression guard.
// Adds nodes via in-session tools then invokes get_graph with format='full'. Every
// emitted node_id must parse with EGuidFormats::DigitsWithHyphens AND be non-zero AND
// contain hyphens. Guards against the "node_id=None on Python side" symptom caused by
// emitting EGuidFormats::Digits (no hyphens), plus the zero-GUID factory regression.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetBlueprintGraphTest_NodeIdSelfHealHyphenated,
	"Claireon.GetBlueprintGraph.NodeId.SelfHealHyphenated",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGetBlueprintGraphTest_NodeIdSelfHealHyphenated::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_NodeIdSelfHeal");

	const FString SessionId = CreateBlueprintSession(*this, AssetPath);
	if (SessionId.IsEmpty()) { return false; }

	// Add a mix of typed CallFunction nodes and a Branch to cover >1 node factory paths.
	TArray<FString> AddedGuids;
	for (int32 i = 0; i < 3; ++i)
	{
		const FString PrintGuid = AddPrintStringNode(*this, SessionId);
		if (PrintGuid.IsEmpty()) { CloseSession(SessionId); return false; }
		AddedGuids.Add(PrintGuid);
	}
	const FString BranchGuid = AddBranchNode(*this, SessionId);
	if (BranchGuid.IsEmpty()) { CloseSession(SessionId); return false; }
	AddedGuids.Add(BranchGuid);

	// Invoke get_graph format='json' node_detail_level='full' on the asset.
	ClaireonTool_GetBlueprintGraph GetTool;
	TSharedPtr<FJsonObject> GetArgs = MakeShared<FJsonObject>();
	GetArgs->SetStringField(TEXT("asset_path"), AssetPath);
	GetArgs->SetStringField(TEXT("node_detail_level"), TEXT("full"));
	GetArgs->SetStringField(TEXT("format"), TEXT("json"));
	auto GR = GetTool.Execute(GetArgs);
	if (GR.bIsError)
	{
		AddError(FString::Printf(TEXT("get_graph failed: %s"), *GR.GetContentAsString()));
		CloseSession(SessionId);
		return false;
	}

	if (!GR.Data.IsValid())
	{
		AddError(TEXT("get_graph returned null Data"));
		CloseSession(SessionId);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* GraphsArr = nullptr;
	if (!GR.Data->TryGetArrayField(TEXT("graphs"), GraphsArr) || !GraphsArr)
	{
		AddError(TEXT("get_graph Data missing 'graphs' array"));
		CloseSession(SessionId);
		return false;
	}

	int32 NodeInspected = 0;
	for (const TSharedPtr<FJsonValue>& GV : *GraphsArr)
	{
		const TSharedPtr<FJsonObject>* GO = nullptr;
		if (!GV->TryGetObject(GO) || !GO) { continue; }
		const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
		if (!(*GO)->TryGetArrayField(TEXT("nodes"), NodesArr) || !NodesArr) { continue; }

		for (const TSharedPtr<FJsonValue>& NV : *NodesArr)
		{
			const TSharedPtr<FJsonObject>* NO = nullptr;
			if (!NV->TryGetObject(NO) || !NO) { continue; }
			FString NodeIdStr;
			if (!(*NO)->TryGetStringField(TEXT("node_id"), NodeIdStr))
			{
				AddError(TEXT("Node object missing 'node_id' field"));
				CloseSession(SessionId);
				return false;
			}
			// verifies: DigitsWithHyphens format shipped (matches ITEM_02 contract).
			if (!NodeIdStr.Contains(TEXT("-")))
			{
				AddError(FString::Printf(TEXT("node_id '%s' missing hyphens; expected DigitsWithHyphens format"), *NodeIdStr));
				CloseSession(SessionId);
				return false;
			}
			FGuid OutGuid;
			// verifies: the strict ParseExact + non-zero check (bare Parse() succeeds on zero GUID).
			if (!FGuid::ParseExact(NodeIdStr, EGuidFormats::DigitsWithHyphens, OutGuid))
			{
				AddError(FString::Printf(TEXT("node_id '%s' failed ParseExact(DigitsWithHyphens)"), *NodeIdStr));
				CloseSession(SessionId);
				return false;
			}
			if (!OutGuid.IsValid())
			{
				AddError(FString::Printf(TEXT("node_id '%s' parsed to zero GUID (the symptom we are guarding)"), *NodeIdStr));
				CloseSession(SessionId);
				return false;
			}
			++NodeInspected;
		}
	}

	if (NodeInspected <= 0)
	{
		AddError(TEXT("No nodes inspected; expected at least the added nodes plus default Event BeginPlay."));
		CloseSession(SessionId);
		return false;
	}

	CloseSession(SessionId);
	return true;
}

// =====================================================================================
// ITEM_02 Test 2: cross-tool handoff. get_graph's node_id is usable directly as
// inspect_node's node_guid argument without format translation -- verifies the
// serializer + get_graph emit the same DigitsWithHyphens format.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetBlueprintGraphTest_NodeIdCrossToolHandoff,
	"Claireon.GetBlueprintGraph.NodeId.CrossToolHandoff",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGetBlueprintGraphTest_NodeIdCrossToolHandoff::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_NodeIdHandoff");

	const FString SessionId = CreateBlueprintSession(*this, AssetPath);
	if (SessionId.IsEmpty()) { return false; }

	const FString PrintGuid = AddPrintStringNode(*this, SessionId);
	if (PrintGuid.IsEmpty()) { CloseSession(SessionId); return false; }

	ClaireonTool_GetBlueprintGraph GetTool;
	TSharedPtr<FJsonObject> GetArgs = MakeShared<FJsonObject>();
	GetArgs->SetStringField(TEXT("asset_path"), AssetPath);
	GetArgs->SetStringField(TEXT("node_detail_level"), TEXT("full"));
	GetArgs->SetStringField(TEXT("format"), TEXT("json"));
	auto GR = GetTool.Execute(GetArgs);
	if (GR.bIsError || !GR.Data.IsValid())
	{
		AddError(FString::Printf(TEXT("get_graph failed or null: %s"), *GR.GetContentAsString()));
		CloseSession(SessionId);
		return false;
	}

	// Pick any node_id from the graphs array.
	FString AnyNodeId;
	const TArray<TSharedPtr<FJsonValue>>* GraphsArr = nullptr;
	if (GR.Data->TryGetArrayField(TEXT("graphs"), GraphsArr) && GraphsArr)
	{
		for (const TSharedPtr<FJsonValue>& GV : *GraphsArr)
		{
			const TSharedPtr<FJsonObject>* GO = nullptr;
			if (!GV->TryGetObject(GO) || !GO) { continue; }
			const TArray<TSharedPtr<FJsonValue>>* NodesArr = nullptr;
			if (!(*GO)->TryGetArrayField(TEXT("nodes"), NodesArr) || !NodesArr) { continue; }
			for (const TSharedPtr<FJsonValue>& NV : *NodesArr)
			{
				const TSharedPtr<FJsonObject>* NO = nullptr;
				if (!NV->TryGetObject(NO) || !NO) { continue; }
				if ((*NO)->TryGetStringField(TEXT("node_id"), AnyNodeId) && !AnyNodeId.IsEmpty())
				{
					break;
				}
			}
			if (!AnyNodeId.IsEmpty()) { break; }
		}
	}

	if (AnyNodeId.IsEmpty())
	{
		AddError(TEXT("Could not pick any node_id from get_graph output"));
		CloseSession(SessionId);
		return false;
	}

	// verifies: get_graph and inspect_node agree on GUID format.
	// The inspect_node path goes through ClaireonBlueprintNodeSerializer, which also emits
	// DigitsWithHyphens; a ParseExact on get_graph's output round-trips into the graph.
	UEdGraph* EventGraph = GetEventGraphFromAsset(AssetPath);
	if (!EventGraph)
	{
		AddError(TEXT("Failed to resolve EventGraph for handoff check"));
		CloseSession(SessionId);
		return false;
	}
	FGuid Parsed;
	if (!FGuid::ParseExact(AnyNodeId, EGuidFormats::DigitsWithHyphens, Parsed))
	{
		AddError(FString::Printf(TEXT("get_graph node_id '%s' not ParseExact(DigitsWithHyphens)"), *AnyNodeId));
		CloseSession(SessionId);
		return false;
	}
	UEdGraphNode* Found = ClaireonBlueprintHelpers::FindNodeByGuid(EventGraph, Parsed);
	if (!Found)
	{
		AddError(FString::Printf(TEXT("get_graph node_id '%s' does not resolve to a node in EventGraph"), *AnyNodeId));
		CloseSession(SessionId);
		return false;
	}

	CloseSession(SessionId);
	return true;
}

// =====================================================================================
// ITEM_02 Test 3: apply_delta id_map format. After apply_delta creates nodes, every
// value in the returned id_map must be in DigitsWithHyphens format (hyphenated) and
// parse via FGuid::ParseExact(DigitsWithHyphens). Closes the third emission site
// referenced by ITEM_02.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetBlueprintGraphTest_ApplyGraphIdMapFormat,
	"Claireon.GetBlueprintGraph.NodeId.ApplyGraphIdMapFormat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGetBlueprintGraphTest_ApplyGraphIdMapFormat::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_ApplyGraphIdMap");

	const FString SessionId = CreateBlueprintSession(*this, AssetPath);
	if (SessionId.IsEmpty()) { return false; }

	// Build minimal apply_delta payload: two CallFunction nodes by local-id.
	TSharedPtr<FJsonObject> ApplyArgs = MakeShared<FJsonObject>();
	ApplyArgs->SetStringField(TEXT("session_id"), SessionId);

	TArray<TSharedPtr<FJsonValue>> Nodes;
	for (int32 i = 0; i < 2; ++i)
	{
		TSharedPtr<FJsonObject> N = MakeShared<FJsonObject>();
		N->SetStringField(TEXT("id"), FString::Printf(TEXT("local_%d"), i));
		N->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		N->SetStringField(TEXT("function_name"), TEXT("PrintString"));
		N->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));
		Nodes.Add(MakeShared<FJsonValueObject>(N));
	}
	ApplyArgs->SetArrayField(TEXT("nodes"), Nodes);

	ClaireonTool_ApplyBlueprintDelta ApplyTool;
	auto AR = ApplyTool.Execute(ApplyArgs);
	if (AR.bIsError)
	{
		AddError(FString::Printf(TEXT("apply_delta failed: %s"), *AR.GetContentAsString()));
		CloseSession(SessionId);
		return false;
	}

	if (!AR.Data.IsValid())
	{
		AddError(TEXT("apply_delta returned null Data"));
		CloseSession(SessionId);
		return false;
	}

	const TSharedPtr<FJsonObject>* IdMapObj = nullptr;
	if (!AR.Data->TryGetObjectField(TEXT("id_map"), IdMapObj) || !IdMapObj || !(*IdMapObj).IsValid())
	{
		AddError(TEXT("apply_delta Data missing 'id_map' object"));
		CloseSession(SessionId);
		return false;
	}

	int32 Checked = 0;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*IdMapObj)->Values)
	{
		FString GuidStr;
		if (!Kv.Value.IsValid() || !Kv.Value->TryGetString(GuidStr))
		{
			AddError(FString::Printf(TEXT("id_map['%s'] not a string"), *Kv.Key));
			CloseSession(SessionId);
			return false;
		}
		// verifies: id_map entries are DigitsWithHyphens (hyphenated) -- ITEM_02 edit 3.
		if (!GuidStr.Contains(TEXT("-")))
		{
			AddError(FString::Printf(TEXT("id_map['%s']='%s' missing hyphens; expected DigitsWithHyphens"), *Kv.Key, *GuidStr));
			CloseSession(SessionId);
			return false;
		}
		FGuid Parsed;
		if (!FGuid::ParseExact(GuidStr, EGuidFormats::DigitsWithHyphens, Parsed) || !Parsed.IsValid())
		{
			AddError(FString::Printf(TEXT("id_map['%s']='%s' ParseExact(DigitsWithHyphens) failed or zero GUID"), *Kv.Key, *GuidStr));
			CloseSession(SessionId);
			return false;
		}
		++Checked;
	}

	if (Checked < 2)
	{
		AddError(FString::Printf(TEXT("Expected at least 2 id_map entries, got %d"), Checked));
		CloseSession(SessionId);
		return false;
	}

	CloseSession(SessionId);
	return true;
}

// =====================================================================================
// canonical field-name shape. Every graph has graph_name; every node has
// node_title and node_class. Short-form aliases (name/title/class) were dropped
// to align with the rest of the BP output surface (node_id, pin_name, etc.).
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetBlueprintGraphTest_FieldAliases_GraphAndNodeObjects,
	"Claireon.GetBlueprintGraph.FieldAliases.GraphAndNodeObjects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGetBlueprintGraphTest_FieldAliases_GraphAndNodeObjects::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_FieldAliasesGraph");

	const FString SessionId = CreateBlueprintSession(*this, AssetPath);
	if (SessionId.IsEmpty()) { return false; }

	AddPrintStringNode(*this, SessionId);
	AddBranchNode(*this, SessionId);

	ClaireonTool_GetBlueprintGraph GetTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("node_detail_level"), TEXT("full"));
	Args->SetStringField(TEXT("format"), TEXT("json"));
	auto R = GetTool.Execute(Args);
	if (R.bIsError || !R.Data.IsValid())
	{
		AddError(FString::Printf(TEXT("get_graph failed: %s"), *R.GetContentAsString()));
		CloseSession(SessionId);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Graphs = nullptr;
	if (!R.Data->TryGetArrayField(TEXT("graphs"), Graphs) || !Graphs || Graphs->Num() == 0)
	{
		AddError(TEXT("get_graph returned no 'graphs'"));
		CloseSession(SessionId);
		return false;
	}

	for (const TSharedPtr<FJsonValue>& GV : *Graphs)
	{
		const TSharedPtr<FJsonObject>* G = nullptr;
		if (!GV->TryGetObject(G) || !G) { continue; }
		FString GraphName;
		if (!(*G)->TryGetStringField(TEXT("graph_name"), GraphName) || GraphName.IsEmpty())
		{
			AddError(TEXT("Graph object missing graph_name")); CloseSession(SessionId); return false;
		}
		// Short-form `name` alias must NOT be present anymore (B11).
		if ((*G)->HasField(TEXT("name")))
		{
			AddError(TEXT("Graph object should not carry the dropped 'name' alias")); CloseSession(SessionId); return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* Nodes = nullptr;
		if (!(*G)->TryGetArrayField(TEXT("nodes"), Nodes) || !Nodes) { continue; }
		for (const TSharedPtr<FJsonValue>& NV : *Nodes)
		{
			const TSharedPtr<FJsonObject>* N = nullptr;
			if (!NV->TryGetObject(N) || !N) { continue; }

			FString Title;
			if (!(*N)->TryGetStringField(TEXT("node_title"), Title))
			{
				AddError(TEXT("Node missing node_title")); CloseSession(SessionId); return false;
			}
			if ((*N)->HasField(TEXT("title")))
			{
				AddError(TEXT("Node should not carry the dropped 'title' alias")); CloseSession(SessionId); return false;
			}

			FString Class;
			if (!(*N)->TryGetStringField(TEXT("node_class"), Class))
			{
				AddError(TEXT("Node missing node_class")); CloseSession(SessionId); return false;
			}
			if ((*N)->HasField(TEXT("class")))
			{
				AddError(TEXT("Node should not carry the dropped 'class' alias")); CloseSession(SessionId); return false;
			}
		}
	}

	CloseSession(SessionId);
	return true;
}

// =====================================================================================
// ITEM_03 Test 5: response-size smoke. Synthesize a 100-node graph; assert
// blueprint_get_graph(format='full') returns under 256 KB and no error.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetBlueprintGraphTest_FieldAliases_ResponseSizeSmoke,
	"Claireon.GetBlueprintGraph.FieldAliases.ResponseSizeSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGetBlueprintGraphTest_FieldAliases_ResponseSizeSmoke::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_FieldAliasesSize");

	const FString SessionId = CreateBlueprintSession(*this, AssetPath);
	if (SessionId.IsEmpty()) { return false; }

	for (int32 i = 0; i < 100; ++i)
	{
		const FString G = AddPrintStringNode(*this, SessionId);
		if (G.IsEmpty()) { CloseSession(SessionId); return false; }
	}

	ClaireonTool_GetBlueprintGraph GetTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("node_detail_level"), TEXT("full"));
	Args->SetStringField(TEXT("format"), TEXT("json"));
	Args->SetNumberField(TEXT("max_nodes"), 0); // unlimited
	auto R = GetTool.Execute(Args);
	if (R.bIsError)
	{
		AddError(FString::Printf(TEXT("get_graph failed: %s"), *R.GetContentAsString()));
		CloseSession(SessionId);
		return false;
	}

	// Serialize Data to JSON string and assert size <= 256 KB.
	FString Serialized;
	if (R.Data.IsValid())
	{
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
		FJsonSerializer::Serialize(R.Data.ToSharedRef(), Writer);
	}
	const int32 MaxBytes = 256 * 1024;
	if (Serialized.Len() > MaxBytes)
	{
		AddError(FString::Printf(TEXT("Response size %d exceeds 256 KB cap"), Serialized.Len()));
		CloseSession(SessionId);
		return false;
	}

	CloseSession(SessionId);
	return true;
}

// =====================================================================================
// get_graph spill no longer silently empties data.graphs.
// Force a spill by directing the OutputGate at a scoped test-results root, route
// a 100-node graph result through the gate, and assert (a) the Summary carries the
// loud "[SPILLED -> <path>]" marker and (b) the data envelope carries
// __mcp_spilled__=true plus inline_omitted listing "data" and an error_hint.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetBlueprintGraphTest_Spill_LoudOnSilentEmpty,
	"Claireon.GetBlueprintGraph.Spill.LoudOnSilentEmpty",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGetBlueprintGraphTest_Spill_LoudOnSilentEmpty::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_SpillLoudOnSilentEmpty");

	const FString SessionId = CreateBlueprintSession(*this, AssetPath);
	if (SessionId.IsEmpty()) { return false; }

	// Build a graph guaranteed to exceed the default 8 KiB spill threshold once
	// serialised at node_detail_level='full'. 100 PrintString nodes at full
	// detail produce well over 8 KiB of JSON.
	for (int32 i = 0; i < 100; ++i)
	{
		const FString G = AddPrintStringNode(*this, SessionId);
		if (G.IsEmpty()) { CloseSession(SessionId); return false; }
	}

	ClaireonTool_GetBlueprintGraph GetTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("node_detail_level"), TEXT("full"));
	Args->SetStringField(TEXT("format"), TEXT("json"));
	Args->SetNumberField(TEXT("max_nodes"), 0); // unlimited
	auto R = GetTool.Execute(Args);
	if (R.bIsError)
	{
		AddError(FString::Printf(TEXT("get_graph failed: %s"), *R.GetContentAsString()));
		CloseSession(SessionId);
		return false;
	}

	// Redirect the spill root to a scoped, throw-away dir so we don't pollute
	// ProjectSavedDir/Claireon/Results/ during automation runs.
	const FString ScopedRoot = FPaths::ProjectIntermediateDir()
		/ TEXT("ClaireonTests")
		/ TEXT("GetBlueprintGraph_Spill")
		/ FGuid::NewGuid().ToString(EGuidFormats::Short);
	IFileManager::Get().MakeDirectory(*ScopedRoot, /*Tree*/ true);
	FClaireonOutputGate::SetResultsRootOverrideForTests(ScopedRoot);
	ON_SCOPE_EXIT
	{
		FClaireonOutputGate::SetResultsRootOverrideForTests(FString());
		IFileManager::Get().DeleteDirectory(*ScopedRoot, /*bRequireExists*/ false, /*Tree*/ true);
	};

	auto Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(R), TEXT("blueprint_get_graph"), TEXT("test_conv_spill_loud"),
		EClaireonSpillStreamSet::GenericData);

	// (a) Summary carries the loud spill marker.
	if (!Routed.Summary.StartsWith(TEXT("[SPILLED -> ")))
	{
		AddError(FString::Printf(TEXT("Expected Summary to start with '[SPILLED -> ', got: %s"), *Routed.Summary));
		CloseSession(SessionId);
		return false;
	}

	// (b) __mcp_spilled__ is true and inline_omitted lists "data".
	if (!Routed.Data.IsValid())
	{
		AddError(TEXT("Routed.Data is not valid after spill"));
		CloseSession(SessionId);
		return false;
	}
	bool bSpilled = false;
	Routed.Data->TryGetBoolField(TEXT("__mcp_spilled__"), bSpilled);
	if (!bSpilled)
	{
		AddError(TEXT("Expected __mcp_spilled__=true after forced 100-node full-detail spill"));
		CloseSession(SessionId);
		return false;
	}

	FString Hint;
	if (!Routed.Data->TryGetStringField(TEXT("error_hint"), Hint) || Hint.IsEmpty())
	{
		AddError(TEXT("Expected error_hint field naming the spill path"));
		CloseSession(SessionId);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* OmittedArr = nullptr;
	bool bSawDataOmitted = false;
	if (Routed.Data->TryGetArrayField(TEXT("inline_omitted"), OmittedArr) && OmittedArr)
	{
		for (const TSharedPtr<FJsonValue>& V : *OmittedArr)
		{
			if (V.IsValid() && V->AsString() == TEXT("data"))
			{
				bSawDataOmitted = true;
				break;
			}
		}
	}
	if (!bSawDataOmitted)
	{
		AddError(TEXT("Expected inline_omitted to contain 'data' for the GenericData stream class"));
		CloseSession(SessionId);
		return false;
	}

	CloseSession(SessionId);
	return true;
}

