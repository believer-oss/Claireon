// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Stage 029 rewrite: setup helpers now use the decomposed
// ClaireonBlueprintGraphTool_* classes instead of the deleted
// ClaireonTool_EditBlueprintGraph shim. The tool-under-test
// (ClaireonTool_GetBlueprintGraph) is untouched.

#include "Tools/ClaireonTool_GetBlueprintGraph.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/ClaireonBlueprintGraphTool_MoveNode.h"
#include "Tools/ClaireonBlueprintGraphTool_Close.h"
#include "ClaireonBlueprintHelpers.h"

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Internationalization/Regex.h"

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
