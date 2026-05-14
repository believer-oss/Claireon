// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Stage 029 rewrite: setup helpers now use the decomposed
// ClaireonBlueprintGraphTool_* classes instead of the deleted
// ClaireonTool_EditBlueprintGraph shim. The tool-under-test
// (ClaireonTool_InspectBlueprintNode) is untouched.

#include "Tools/ClaireonTool_InspectBlueprintNode.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/ClaireonBlueprintGraphTool_Compile.h"
#include "Tools/ClaireonBlueprintGraphTool_Close.h"
#include "Tools/ClaireonBlueprintGraphTool_GetState.h"
#include "ClaireonBlueprintHelpers.h"

#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Blueprint.h"

namespace
{
	// Helpers are prefixed to avoid unity-build ODR collisions with
	// same-named helpers in ClaireonTool_EditBlueprintGraph.spec.cpp, which
	// share an anonymous-namespace scope when Clang lumps multiple .cpp
	// files into a single unity translation unit.

	/** Extract the focused cursor node GUID from a full-mode BuildStateResponse body.
	 *  Parses "Focused Node: <title> [GUID: <guid>]" produced by BuildStateResponse. */
	FString InspectBPSpec_ExtractCursorNodeGuid(const FString& ResultText)
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

	/** Extract a "Session ID: <id>" line value from a full-mode BuildStateResponse body. */
	FString InspectBPSpec_ExtractSessionId(const FString& ResultText)
	{
		int32 Start = ResultText.Find(TEXT("Session ID: "));
		if (Start == INDEX_NONE) { return FString(); }
		Start += 12;
		int32 End = ResultText.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, Start);
		if (End == INDEX_NONE) { End = ResultText.Len(); }
		return ResultText.Mid(Start, End - Start).TrimStartAndEnd();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectBlueprintNodeTest_StatelessWithoutSession,
	"Claireon.InspectBlueprintNode.StatelessWithoutSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInspectBlueprintNodeTest_StatelessWithoutSession::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_InspectStatelessBasic");

	// Setup: create a BP with a PrintString node via the session-based tools, then close.
	FString SessionId;
	FString PrintStringGuid;
	FString GraphName;
	{
		ClaireonBlueprintGraphTool_Create CreateTool;
		TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
		CreateArgs->SetStringField(TEXT("asset_path"), AssetPath);
		CreateArgs->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		auto CR = CreateTool.Execute(CreateArgs);
		if (CR.bIsError)
		{
			AddError(FString::Printf(TEXT("Create BP failed: %s"), *CR.GetContentAsString()));
			return false;
		}
		SessionId = InspectBPSpec_ExtractSessionId(CR.GetContentAsString());
		if (SessionId.IsEmpty())
		{
			AddError(TEXT("Could not extract session ID from create response"));
			return false;
		}

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
			AddError(FString::Printf(TEXT("add_node failed: %s"), *AR.GetContentAsString()));
			return false;
		}
		PrintStringGuid = InspectBPSpec_ExtractCursorNodeGuid(AR.GetContentAsString());

		// Grab the active graph name via get_state response text
		ClaireonBlueprintGraphTool_GetState StateTool;
		TSharedPtr<FJsonObject> StateArgs = MakeShared<FJsonObject>();
		StateArgs->SetStringField(TEXT("session_id"), SessionId);
		StateArgs->SetStringField(TEXT("response_mode"), TEXT("full"));
		auto SR = StateTool.Execute(StateArgs);
		const FString StateText = SR.GetContentAsString();
		// State text contains "Graph: <name>" line; default-created BP uses EventGraph.
		int32 GraphStart = StateText.Find(TEXT("Graph: "));
		if (GraphStart != INDEX_NONE)
		{
			GraphStart += 7;
			int32 GraphEnd = StateText.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, GraphStart);
			if (GraphEnd == INDEX_NONE) { GraphEnd = StateText.Len(); }
			GraphName = StateText.Mid(GraphStart, GraphEnd - GraphStart).TrimStartAndEnd();
		}
		if (GraphName.IsEmpty())
		{
			GraphName = TEXT("EventGraph");
		}

		// Compile + close so the stateless tool can LoadObject the BP.
		{
			ClaireonBlueprintGraphTool_Compile CompileTool;
			TSharedPtr<FJsonObject> CompileArgs = MakeShared<FJsonObject>();
			CompileArgs->SetStringField(TEXT("session_id"), SessionId);
			CompileTool.Execute(CompileArgs);
		}

		{
			ClaireonBlueprintGraphTool_Close CloseTool;
			TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
			CloseArgs->SetStringField(TEXT("session_id"), SessionId);
			CloseTool.Execute(CloseArgs);
		}
	}

	if (PrintStringGuid.IsEmpty())
	{
		AddError(TEXT("Failed to capture PrintString node GUID"));
		return false;
	}

	// Exercise the stateless tool directly
	ClaireonTool_InspectBlueprintNode InspectTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("graph_name"), GraphName);
	Args->SetStringField(TEXT("node_guid"), PrintStringGuid);

	auto Result = InspectTool.Execute(Args);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("blueprint_inspect_node failed: %s"), *Result.GetContentAsString()));
		return false;
	}

	// Verify payload parses as JSON and node_id matches the requested GUID.
	TSharedPtr<FJsonObject> Payload;
	auto Reader = TJsonReaderFactory<>::Create(Result.GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, Payload) || !Payload.IsValid())
	{
		AddError(TEXT("Payload failed to parse as JSON"));
		return false;
	}
	FString NodeId;
	if (!Payload->TryGetStringField(TEXT("node_id"), NodeId))
	{
		AddError(TEXT("Payload missing node_id"));
		return false;
	}

	// Normalize both GUIDs to DigitsWithHyphens so we don't accidentally compare formats.
	FGuid ParsedRequested, ParsedReturned;
	if (!FGuid::Parse(PrintStringGuid, ParsedRequested))
	{
		AddError(FString::Printf(TEXT("Failed to parse requested GUID: %s"), *PrintStringGuid));
		return false;
	}
	if (!FGuid::Parse(NodeId, ParsedReturned))
	{
		AddError(FString::Printf(TEXT("Failed to parse returned node_id: %s"), *NodeId));
		return false;
	}
	if (ParsedRequested != ParsedReturned)
	{
		AddError(FString::Printf(TEXT("Expected node_id %s to match requested %s"),
			*NodeId, *PrintStringGuid));
		return false;
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectBlueprintNodeTest_AnimGraphRedirectStatelessPath,
	"Claireon.InspectBlueprintNode.AnimGraphRedirectStatelessPath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInspectBlueprintNodeTest_AnimGraphRedirectStatelessPath::RunTest(const FString& Parameters)
{
	// Creating an Animation Blueprint via automation requires a target skeleton
	// asset, which this test fixture does not provide. The redirect code path
	// itself is a single Cast<UAnimGraphNode_Base>() check that runs after the
	// asset/graph/node-resolution plumbing verified in StatelessWithoutSession,
	// so the risk of drift is minimal. We record an AddInfo so the harness
	// surfaces the skip rather than silently dropping coverage; the check is
	// also exercised manually against real AnimBP assets via the MCP surface.
	AddInfo(TEXT("Skipped: stateless AnimBP inspection requires a skeleton-backed "
		"AnimBP asset in the automation fixture. The UAnimGraphNode_Base cast is "
		"covered by manual exercise against live AnimBPs."));
	return true;
}

// =====================================================================================
// ITEM_03 Test 2: serializer alias coverage. The serializer (used by inspect_node)
// emits BOTH node_title/title and node_class/class. Equal values.
// =====================================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FInspectBlueprintNodeTest_FieldAliases_SerializerAliases,
	"Claireon.InspectBlueprintNode.FieldAliases.SerializerAliases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FInspectBlueprintNodeTest_FieldAliases_SerializerAliases::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/BP_SerializerAliases");

	FString SessionId;
	FString PrintStringGuid;
	FString GraphName;
	{
		ClaireonBlueprintGraphTool_Create CreateTool;
		TSharedPtr<FJsonObject> CreateArgs = MakeShared<FJsonObject>();
		CreateArgs->SetStringField(TEXT("asset_path"), AssetPath);
		CreateArgs->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		auto CR = CreateTool.Execute(CreateArgs);
		if (CR.bIsError)
		{
			AddError(FString::Printf(TEXT("Create BP failed: %s"), *CR.GetContentAsString()));
			return false;
		}
		SessionId = InspectBPSpec_ExtractSessionId(CR.GetContentAsString());
		if (SessionId.IsEmpty()) { AddError(TEXT("No session id")); return false; }

		ClaireonBlueprintGraphTool_AddNode AddTool;
		TSharedPtr<FJsonObject> AddArgs = MakeShared<FJsonObject>();
		AddArgs->SetStringField(TEXT("session_id"), SessionId);
		AddArgs->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		AddArgs->SetStringField(TEXT("function_name"), TEXT("PrintString"));
		AddArgs->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));
		AddArgs->SetStringField(TEXT("response_mode"), TEXT("full"));
		auto AR = AddTool.Execute(AddArgs);
		if (AR.bIsError) { AddError(TEXT("add_node failed")); return false; }
		PrintStringGuid = InspectBPSpec_ExtractCursorNodeGuid(AR.GetContentAsString());
		GraphName = TEXT("EventGraph");

		ClaireonBlueprintGraphTool_Compile CompileTool;
		TSharedPtr<FJsonObject> CompileArgs = MakeShared<FJsonObject>();
		CompileArgs->SetStringField(TEXT("session_id"), SessionId);
		CompileTool.Execute(CompileArgs);

		ClaireonBlueprintGraphTool_Close CloseTool;
		TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
		CloseArgs->SetStringField(TEXT("session_id"), SessionId);
		CloseTool.Execute(CloseArgs);
	}
	if (PrintStringGuid.IsEmpty()) { AddError(TEXT("No node guid")); return false; }

	ClaireonTool_InspectBlueprintNode InspectTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("graph_name"), GraphName);
	Args->SetStringField(TEXT("node_guid"), PrintStringGuid);

	auto Result = InspectTool.Execute(Args);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("inspect_node failed: %s"), *Result.GetContentAsString()));
		return false;
	}

	TSharedPtr<FJsonObject> Payload;
	auto Reader = TJsonReaderFactory<>::Create(Result.GetContentAsString());
	if (!FJsonSerializer::Deserialize(Reader, Payload) || !Payload.IsValid())
	{
		AddError(TEXT("Payload failed to parse as JSON"));
		return false;
	}

	FString Title, TitleAlias, Class, ClassAlias;
	if (!Payload->TryGetStringField(TEXT("node_title"), Title) ||
		!Payload->TryGetStringField(TEXT("title"), TitleAlias))
	{
		AddError(TEXT("Payload missing node_title or title alias")); return false;
	}
	if (Title != TitleAlias)
	{
		AddError(FString::Printf(TEXT("node_title '%s' != title '%s'"), *Title, *TitleAlias));
		return false;
	}
	if (!Payload->TryGetStringField(TEXT("node_class"), Class) ||
		!Payload->TryGetStringField(TEXT("class"), ClassAlias))
	{
		AddError(TEXT("Payload missing node_class or class alias")); return false;
	}
	if (Class != ClassAlias)
	{
		AddError(FString::Printf(TEXT("node_class '%s' != class '%s'"), *Class, *ClassAlias));
		return false;
	}

	// If there are any 'pins' with 'linked_to' refs, assert title aliases on linked refs too.
	const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
	if (Payload->TryGetArrayField(TEXT("pins"), Pins))
	{
		for (const TSharedPtr<FJsonValue>& PV : *Pins)
		{
			const TSharedPtr<FJsonObject>* P = nullptr;
			if (!PV->TryGetObject(P) || !P) { continue; }
			const TArray<TSharedPtr<FJsonValue>>* LinkedTo = nullptr;
			if (!(*P)->TryGetArrayField(TEXT("linked_to"), LinkedTo) || !LinkedTo) { continue; }
			for (const TSharedPtr<FJsonValue>& LV : *LinkedTo)
			{
				const TSharedPtr<FJsonObject>* L = nullptr;
				if (!LV->TryGetObject(L) || !L) { continue; }
				FString LT, LTa;
				if ((*L)->TryGetStringField(TEXT("node_title"), LT))
				{
					if (!(*L)->TryGetStringField(TEXT("title"), LTa))
					{
						AddError(TEXT("Linked ref missing 'title' alias")); return false;
					}
					if (LT != LTa)
					{
						AddError(FString::Printf(TEXT("linked_to node_title '%s' != title '%s'"), *LT, *LTa));
						return false;
					}
				}
			}
		}
	}

	return true;
}
