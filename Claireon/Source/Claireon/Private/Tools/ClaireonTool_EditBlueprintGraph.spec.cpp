// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_EditBlueprintGraph.h"
#include "Tools/ClaireonTool_BlueprintCompile.h"
#include "ClaireonLog.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditBlueprintGraphTest_CreateAndBasicOps,
	"Claireon.EditBlueprintGraph.CreateAndBasicOps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditBlueprintGraphTest_CreateAndBasicOps::RunTest(const FString& Parameters)
{
	ClaireonTool_EditBlueprintGraph Tool;

	// Test 1: Create a new Blueprint
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("operation"), TEXT("create"));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_TestActor"));
		Params->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		CreateParams->SetObjectField(TEXT("params"), Params);

		auto Result = Tool.Execute(CreateParams);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to create blueprint: %s"), *Result.GetContentAsString()));
			return false;
		}

		// Extract session ID from result
		FString ResultText = Result.GetContentAsString();
		FString SessionId;

		// Parse session ID from result
		int32 SessionIdStart = ResultText.Find(TEXT("Session ID: "));
		if (SessionIdStart != INDEX_NONE)
		{
			SessionIdStart += 12; // Length of "Session ID: "
			int32 SessionIdEnd = ResultText.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SessionIdStart);
			SessionId = ResultText.Mid(SessionIdStart, SessionIdEnd - SessionIdStart);
		}

		if (SessionId.IsEmpty())
		{
			AddError(TEXT("Failed to extract session ID from create result"));
			return false;
		}

		AddInfo(FString::Printf(TEXT("Created blueprint with session: %s"), *SessionId));

		// Test 2: Add a PrintString node using add_node (now with correct type name!)
		{
			TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
			AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
			AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

			TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
			NodeParams->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
			NodeParams->SetStringField(TEXT("function_name"), TEXT("PrintString"));
			NodeParams->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));
			NodeParams->SetBoolField(TEXT("auto_connect_from_cursor"), true);
			AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

			Result = Tool.Execute(AddNodeArgs);
			if (Result.bIsError)
			{
				AddError(FString::Printf(TEXT("Failed to add PrintString node: %s"), *Result.GetContentAsString()));
				return false;
			}

			AddInfo(TEXT("Successfully added PrintString node"));
		}

		// Test 3: Add a Branch node
		{
			TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
			AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
			AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

			TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
			NodeParams->SetStringField(TEXT("node_type"), TEXT("Branch"));
			AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

			Result = Tool.Execute(AddNodeArgs);
			if (Result.bIsError)
			{
				AddError(FString::Printf(TEXT("Failed to add Branch node: %s"), *Result.GetContentAsString()));
				return false;
			}

			AddInfo(TEXT("Successfully added Branch node"));
		}

		// Test 4: Connect pins using node title (not GUID!)
		{
			TSharedPtr<FJsonObject> ConnectArgs = MakeShared<FJsonObject>();
			ConnectArgs->SetStringField(TEXT("operation"), TEXT("connect_pins"));
			ConnectArgs->SetStringField(TEXT("session_id"), SessionId);

			TSharedPtr<FJsonObject> ConnectParams = MakeShared<FJsonObject>();
			ConnectParams->SetStringField(TEXT("source_node_title"), TEXT("Event BeginPlay"));
			ConnectParams->SetStringField(TEXT("source_pin_name"), TEXT("then"));
			ConnectParams->SetStringField(TEXT("target_node_title"), TEXT("Print String"));
			ConnectParams->SetStringField(TEXT("target_pin_name"), TEXT("execute"));
			ConnectArgs->SetObjectField(TEXT("params"), ConnectParams);

			Result = Tool.Execute(ConnectArgs);
			if (Result.bIsError)
			{
				AddError(FString::Printf(TEXT("Failed to connect pins by title: %s"), *Result.GetContentAsString()));
				return false;
			}

			AddInfo(TEXT("Successfully connected pins using node titles (no GUIDs!)"));
		}

		// Test 5: Compile
		{
			TSharedPtr<FJsonObject> CompileArgs = MakeShared<FJsonObject>();
			CompileArgs->SetStringField(TEXT("operation"), TEXT("compile"));
			CompileArgs->SetStringField(TEXT("session_id"), SessionId);
			CompileArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

			Result = Tool.Execute(CompileArgs);
			if (Result.bIsError)
			{
				AddError(FString::Printf(TEXT("Failed to compile: %s"), *Result.GetContentAsString()));
				return false;
			}

			AddInfo(TEXT("Successfully compiled blueprint"));
		}

		// Test 6: Save
		{
			TSharedPtr<FJsonObject> SaveArgs = MakeShared<FJsonObject>();
			SaveArgs->SetStringField(TEXT("operation"), TEXT("save"));
			SaveArgs->SetStringField(TEXT("session_id"), SessionId);
			SaveArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

			Result = Tool.Execute(SaveArgs);
			if (Result.bIsError)
			{
				AddError(FString::Printf(TEXT("Failed to save: %s"), *Result.GetContentAsString()));
				return false;
			}

			AddInfo(TEXT("Successfully saved blueprint"));
		}

		// Test 7: Close session
		{
			TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
			CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
			CloseArgs->SetStringField(TEXT("session_id"), SessionId);
			CloseArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

			Result = Tool.Execute(CloseArgs);
			if (Result.bIsError)
			{
				AddError(FString::Printf(TEXT("Failed to close session: %s"), *Result.GetContentAsString()));
				return false;
			}

			AddInfo(TEXT("Successfully closed session"));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditBlueprintGraphTest_NodeTypes,
	"Claireon.EditBlueprintGraph.NodeTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditBlueprintGraphTest_NodeTypes::RunTest(const FString& Parameters)
{
	ClaireonTool_EditBlueprintGraph Tool;

	// Create a test blueprint
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("operation"), TEXT("create"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_NodeTypeTest"));
	Params->SetStringField(TEXT("parent_class"), TEXT("Actor"));
	CreateParams->SetObjectField(TEXT("params"), Params);

	auto Result = Tool.Execute(CreateParams);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Failed to create test blueprint: %s"), *Result.GetContentAsString()));
		return false;
	}

	// Extract session ID
	FString ResultText = Result.GetContentAsString();
	FString SessionId;
	int32 SessionIdStart = ResultText.Find(TEXT("Session ID: "));
	if (SessionIdStart != INDEX_NONE)
	{
		SessionIdStart += 12;
		int32 SessionIdEnd = ResultText.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SessionIdStart);
		SessionId = ResultText.Mid(SessionIdStart, SessionIdEnd - SessionIdStart);
	}

	if (SessionId.IsEmpty())
	{
		AddError(TEXT("Failed to extract session ID"));
		return false;
	}

	// Test each node type
	struct FNodeTypeTest
	{
		FString NodeType;
		TMap<FString, FString> RequiredParams;
		FString Description;
	};

	TArray<FNodeTypeTest> NodeTypesToTest = {
		{ TEXT("Branch"), {}, TEXT("Branch (if-then-else)") },
		{ TEXT("Sequence"), {}, TEXT("Sequence") },
		{ TEXT("Select"), {}, TEXT("Select") },
		{ TEXT("Knot"), {}, TEXT("Reroute node") },
		{ TEXT("Comment"), { {TEXT("comment_text"), TEXT("Test Comment")} }, TEXT("Comment") },
		{ TEXT("ForEachLoop"), {}, TEXT("ForEachLoop") },
		{ TEXT("MakeArray"), {}, TEXT("Make Array") },
		{ TEXT("MakeSet"), {}, TEXT("Make Set") },
		{ TEXT("MakeMap"), {}, TEXT("Make Map") },
		{ TEXT("VariableGet"), { {TEXT("variable_name"), TEXT("TestVar")} }, TEXT("Get Variable") },
		{ TEXT("VariableSet"), { {TEXT("variable_name"), TEXT("TestVar")} }, TEXT("Set Variable") },
	};

	for (const FNodeTypeTest& Test : NodeTypesToTest)
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), Test.NodeType);

		// Add required params
		for (const auto& Pair : Test.RequiredParams)
		{
			NodeParams->SetStringField(Pair.Key, Pair.Value);
		}

		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add %s node: %s"), *Test.Description, *Result.GetContentAsString()));
			return false;
		}

		AddInfo(FString::Printf(TEXT("✓ Successfully added %s node"), *Test.Description));
	}

	// Test Generic node type
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("Generic"));
		NodeParams->SetStringField(TEXT("class_name"), TEXT("K2Node_AddPinInterface"));
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add Generic node: %s"), *Result.GetContentAsString()));
			return false;
		}

		AddInfo(TEXT("✓ Successfully added Generic node type"));
	}

	// Close session
	{
		TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
		CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
		CloseArgs->SetStringField(TEXT("session_id"), SessionId);
		CloseArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Tool.Execute(CloseArgs);
	}

	return true;
}

// ============================================================================
// Test: Macro node types
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditBlueprintGraphTest_MacroNodes,
	"Claireon.EditBlueprintGraph.MacroNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditBlueprintGraphTest_MacroNodes::RunTest(const FString& Parameters)
{
	ClaireonTool_EditBlueprintGraph Tool;

	// Create test blueprint
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("operation"), TEXT("create"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_MacroNodeTest"));
	Params->SetStringField(TEXT("parent_class"), TEXT("Actor"));
	CreateParams->SetObjectField(TEXT("params"), Params);

	auto Result = Tool.Execute(CreateParams);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Failed to create test blueprint: %s"), *Result.GetContentAsString()));
		return false;
	}

	FString ResultText = Result.GetContentAsString();
	FString SessionId;
	int32 SessionIdStart = ResultText.Find(TEXT("Session ID: "));
	if (SessionIdStart != INDEX_NONE)
	{
		SessionIdStart += 12;
		int32 SessionIdEnd = ResultText.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SessionIdStart);
		SessionId = ResultText.Mid(SessionIdStart, SessionIdEnd - SessionIdStart);
	}

	if (SessionId.IsEmpty())
	{
		AddError(TEXT("Failed to extract session ID"));
		return false;
	}

	// Test macro aliases
	TArray<FString> MacroNames = {
		TEXT("ForEachLoop"), TEXT("ForEachLoopWithBreak"),
		TEXT("DoOnce"), TEXT("FlipFlop"),
		TEXT("Gate"), TEXT("WhileLoop")
	};

	for (const FString& MacroName : MacroNames)
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), MacroName);
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add %s macro node: %s"), *MacroName, *Result.GetContentAsString()));
			return false;
		}

		AddInfo(FString::Printf(TEXT("✓ Successfully added %s macro node"), *MacroName));
	}

	// Test generic Macro type
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("Macro"));
		NodeParams->SetStringField(TEXT("macro_name"), TEXT("ForLoop"));
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add generic Macro node: %s"), *Result.GetContentAsString()));
			return false;
		}

		AddInfo(TEXT("✓ Successfully added generic Macro node (ForLoop)"));
	}

	// Test error case: non-existent macro
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("Macro"));
		NodeParams->SetStringField(TEXT("macro_name"), TEXT("NonExistentMacro"));
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for non-existent macro but got success"));
			return false;
		}

		AddInfo(TEXT("✓ Correctly returned error for non-existent macro"));
	}

	// Close session
	{
		TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
		CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
		CloseArgs->SetStringField(TEXT("session_id"), SessionId);
		CloseArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Tool.Execute(CloseArgs);
	}

	return true;
}

// ============================================================================
// Test: New K2Node types (Switch*, ForEachElementInEnum, DoOnceMultiInput)
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditBlueprintGraphTest_NewK2NodeTypes,
	"Claireon.EditBlueprintGraph.NewK2NodeTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditBlueprintGraphTest_NewK2NodeTypes::RunTest(const FString& Parameters)
{
	ClaireonTool_EditBlueprintGraph Tool;

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("operation"), TEXT("create"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_NewK2NodeTest"));
	Params->SetStringField(TEXT("parent_class"), TEXT("Actor"));
	CreateParams->SetObjectField(TEXT("params"), Params);

	auto Result = Tool.Execute(CreateParams);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Failed to create test blueprint: %s"), *Result.GetContentAsString()));
		return false;
	}

	FString ResultText = Result.GetContentAsString();
	FString SessionId;
	int32 SessionIdStart = ResultText.Find(TEXT("Session ID: "));
	if (SessionIdStart != INDEX_NONE)
	{
		SessionIdStart += 12;
		int32 SessionIdEnd = ResultText.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SessionIdStart);
		SessionId = ResultText.Mid(SessionIdStart, SessionIdEnd - SessionIdStart);
	}

	if (SessionId.IsEmpty())
	{
		AddError(TEXT("Failed to extract session ID"));
		return false;
	}

	// Test simple node types (no required params)
	TArray<FString> SimpleNodeTypes = {
		TEXT("SwitchInteger"), TEXT("SwitchString"), TEXT("SwitchName"), TEXT("DoOnceMultiInput")
	};

	for (const FString& NodeType : SimpleNodeTypes)
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), NodeType);
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add %s node: %s"), *NodeType, *Result.GetContentAsString()));
			return false;
		}

		AddInfo(FString::Printf(TEXT("✓ Successfully added %s node"), *NodeType));
	}

	// Test SwitchEnum (requires enum_type)
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("SwitchEnum"));
		NodeParams->SetStringField(TEXT("enum_type"), TEXT("ECollisionChannel"));
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add SwitchEnum node: %s"), *Result.GetContentAsString()));
			return false;
		}

		AddInfo(TEXT("✓ Successfully added SwitchEnum node"));
	}

	// Test ForEachElementInEnum (requires enum_type)
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("ForEachElementInEnum"));
		NodeParams->SetStringField(TEXT("enum_type"), TEXT("ECollisionChannel"));
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add ForEachElementInEnum node: %s"), *Result.GetContentAsString()));
			return false;
		}

		AddInfo(TEXT("✓ Successfully added ForEachElementInEnum node"));
	}

	// Close session
	{
		TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
		CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
		CloseArgs->SetStringField(TEXT("session_id"), SessionId);
		CloseArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Tool.Execute(CloseArgs);
	}

	return true;
}

// ============================================================================
// Test: Dynamic pin add/remove operations
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditBlueprintGraphTest_DynamicPins,
	"Claireon.EditBlueprintGraph.DynamicPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditBlueprintGraphTest_DynamicPins::RunTest(const FString& Parameters)
{
	ClaireonTool_EditBlueprintGraph Tool;

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("operation"), TEXT("create"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_DynamicPinTest"));
	Params->SetStringField(TEXT("parent_class"), TEXT("Actor"));
	CreateParams->SetObjectField(TEXT("params"), Params);

	auto Result = Tool.Execute(CreateParams);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Failed to create test blueprint: %s"), *Result.GetContentAsString()));
		return false;
	}

	FString ResultText = Result.GetContentAsString();
	FString SessionId;
	int32 SessionIdStart = ResultText.Find(TEXT("Session ID: "));
	if (SessionIdStart != INDEX_NONE)
	{
		SessionIdStart += 12;
		int32 SessionIdEnd = ResultText.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SessionIdStart);
		SessionId = ResultText.Mid(SessionIdStart, SessionIdEnd - SessionIdStart);
	}

	if (SessionId.IsEmpty())
	{
		AddError(TEXT("Failed to extract session ID"));
		return false;
	}

	// Helper to extract a node GUID from the last add_node result
	auto ExtractNodeGuid = [&](const FString& ResultStr) -> FString
	{
		int32 GuidStart = ResultStr.Find(TEXT("Cursor Node: "));
		if (GuidStart != INDEX_NONE)
		{
			GuidStart += 13;
			int32 GuidEnd = ResultStr.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, GuidStart);
			if (GuidEnd == INDEX_NONE) GuidEnd = ResultStr.Len();
			return ResultStr.Mid(GuidStart, GuidEnd - GuidStart).TrimStartAndEnd();
		}
		return FString();
	};

	// --- Test 1: Sequence node add_pin ---
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("Sequence"));
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add Sequence node: %s"), *Result.GetContentAsString()));
			return false;
		}

		FString SeqGuid = ExtractNodeGuid(Result.GetContentAsString());

		for (int32 i = 0; i < 2; ++i)
		{
			TSharedPtr<FJsonObject> AddPinArgs = MakeShared<FJsonObject>();
			AddPinArgs->SetStringField(TEXT("operation"), TEXT("add_pin"));
			AddPinArgs->SetStringField(TEXT("session_id"), SessionId);

			TSharedPtr<FJsonObject> PinParams = MakeShared<FJsonObject>();
			PinParams->SetStringField(TEXT("node_guid"), SeqGuid);
			AddPinArgs->SetObjectField(TEXT("params"), PinParams);

			Result = Tool.Execute(AddPinArgs);
			if (Result.bIsError)
			{
				AddError(FString::Printf(TEXT("Failed to add pin to Sequence: %s"), *Result.GetContentAsString()));
				return false;
			}
		}

		AddInfo(TEXT("✓ Successfully added pins to Sequence node"));
	}

	// --- Test 2: MakeArray add_pin with count ---
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("MakeArray"));
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add MakeArray node: %s"), *Result.GetContentAsString()));
			return false;
		}

		FString ArrayGuid = ExtractNodeGuid(Result.GetContentAsString());

		TSharedPtr<FJsonObject> AddPinArgs = MakeShared<FJsonObject>();
		AddPinArgs->SetStringField(TEXT("operation"), TEXT("add_pin"));
		AddPinArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> PinParams = MakeShared<FJsonObject>();
		PinParams->SetStringField(TEXT("node_guid"), ArrayGuid);
		PinParams->SetNumberField(TEXT("count"), 3);
		AddPinArgs->SetObjectField(TEXT("params"), PinParams);

		Result = Tool.Execute(AddPinArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add pins to MakeArray: %s"), *Result.GetContentAsString()));
			return false;
		}

		AddInfo(TEXT("✓ Successfully added 3 pins to MakeArray node"));
	}

	// --- Test 3: SwitchString add_pin with pin_value ---
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("SwitchString"));
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add SwitchString node: %s"), *Result.GetContentAsString()));
			return false;
		}

		FString SwitchGuid = ExtractNodeGuid(Result.GetContentAsString());

		TSharedPtr<FJsonObject> AddPinArgs = MakeShared<FJsonObject>();
		AddPinArgs->SetStringField(TEXT("operation"), TEXT("add_pin"));
		AddPinArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> PinParams = MakeShared<FJsonObject>();
		PinParams->SetStringField(TEXT("node_guid"), SwitchGuid);
		PinParams->SetStringField(TEXT("pin_value"), TEXT("MyCase"));
		AddPinArgs->SetObjectField(TEXT("params"), PinParams);

		Result = Tool.Execute(AddPinArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add pin to SwitchString: %s"), *Result.GetContentAsString()));
			return false;
		}

		AddInfo(TEXT("✓ Successfully added named pin 'MyCase' to SwitchString"));
	}

	// --- Test 4: Error case — add_pin on Branch ---
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("Branch"));
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		FString BranchGuid = ExtractNodeGuid(Result.GetContentAsString());

		TSharedPtr<FJsonObject> AddPinArgs = MakeShared<FJsonObject>();
		AddPinArgs->SetStringField(TEXT("operation"), TEXT("add_pin"));
		AddPinArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> PinParams = MakeShared<FJsonObject>();
		PinParams->SetStringField(TEXT("node_guid"), BranchGuid);
		AddPinArgs->SetObjectField(TEXT("params"), PinParams);

		Result = Tool.Execute(AddPinArgs);
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for add_pin on Branch but got success"));
			return false;
		}

		AddInfo(TEXT("✓ Correctly returned error for add_pin on Branch"));
	}

	// --- Test 5: Error case — add_pin on SwitchEnum ---
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("SwitchEnum"));
		NodeParams->SetStringField(TEXT("enum_type"), TEXT("ECollisionChannel"));
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		FString EnumSwitchGuid = ExtractNodeGuid(Result.GetContentAsString());

		TSharedPtr<FJsonObject> AddPinArgs = MakeShared<FJsonObject>();
		AddPinArgs->SetStringField(TEXT("operation"), TEXT("add_pin"));
		AddPinArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> PinParams = MakeShared<FJsonObject>();
		PinParams->SetStringField(TEXT("node_guid"), EnumSwitchGuid);
		AddPinArgs->SetObjectField(TEXT("params"), PinParams);

		Result = Tool.Execute(AddPinArgs);
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for add_pin on SwitchEnum but got success"));
			return false;
		}

		AddInfo(TEXT("✓ Correctly returned error for add_pin on SwitchEnum"));
	}

	// Close session
	{
		TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
		CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
		CloseArgs->SetStringField(TEXT("session_id"), SessionId);
		CloseArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Tool.Execute(CloseArgs);
	}

	return true;
}

// ============================================================================
// Test: num_extra_pins parameter
// ============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditBlueprintGraphTest_NumExtraPins,
	"Claireon.EditBlueprintGraph.NumExtraPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditBlueprintGraphTest_NumExtraPins::RunTest(const FString& Parameters)
{
	ClaireonTool_EditBlueprintGraph Tool;

	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("operation"), TEXT("create"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_NumExtraPinsTest"));
	Params->SetStringField(TEXT("parent_class"), TEXT("Actor"));
	CreateParams->SetObjectField(TEXT("params"), Params);

	auto Result = Tool.Execute(CreateParams);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Failed to create test blueprint: %s"), *Result.GetContentAsString()));
		return false;
	}

	FString ResultText = Result.GetContentAsString();
	FString SessionId;
	int32 SessionIdStart = ResultText.Find(TEXT("Session ID: "));
	if (SessionIdStart != INDEX_NONE)
	{
		SessionIdStart += 12;
		int32 SessionIdEnd = ResultText.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SessionIdStart);
		SessionId = ResultText.Mid(SessionIdStart, SessionIdEnd - SessionIdStart);
	}

	if (SessionId.IsEmpty())
	{
		AddError(TEXT("Failed to extract session ID"));
		return false;
	}

	// Test: Sequence with num_extra_pins=3
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("Sequence"));
		NodeParams->SetNumberField(TEXT("num_extra_pins"), 3);
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add Sequence with extra pins: %s"), *Result.GetContentAsString()));
			return false;
		}

		AddInfo(TEXT("✓ Successfully added Sequence node with num_extra_pins=3"));
	}

	// Test: MakeArray with num_extra_pins=5
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("MakeArray"));
		NodeParams->SetNumberField(TEXT("num_extra_pins"), 5);
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add MakeArray with extra pins: %s"), *Result.GetContentAsString()));
			return false;
		}

		AddInfo(TEXT("✓ Successfully added MakeArray node with num_extra_pins=5"));
	}

	// Test: SwitchInteger with num_extra_pins=4
	{
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("SwitchInteger"));
		NodeParams->SetNumberField(TEXT("num_extra_pins"), 4);
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		Result = Tool.Execute(AddNodeArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add SwitchInteger with extra pins: %s"), *Result.GetContentAsString()));
			return false;
		}

		AddInfo(TEXT("✓ Successfully added SwitchInteger node with num_extra_pins=4"));
	}

	// Close session
	{
		TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
		CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
		CloseArgs->SetStringField(TEXT("session_id"), SessionId);
		CloseArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Tool.Execute(CloseArgs);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditBlueprintGraphTest_ImportNodes,
	"Claireon.EditBlueprintGraph.ImportNodes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditBlueprintGraphTest_ImportNodes::RunTest(const FString& Parameters)
{
	ClaireonTool_EditBlueprintGraph Tool;

	// Create a test blueprint
	TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
	CreateParams->SetStringField(TEXT("operation"), TEXT("create"));

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_ImportTest"));
	Params->SetStringField(TEXT("parent_class"), TEXT("Actor"));
	CreateParams->SetObjectField(TEXT("params"), Params);

	auto Result = Tool.Execute(CreateParams);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("Failed to create blueprint: %s"), *Result.GetContentAsString()));
		return false;
	}

	// Extract session ID
	FString ResultText = Result.GetContentAsString();
	FString SessionId;
	int32 SessionIdStart = ResultText.Find(TEXT("Session ID: "));
	if (SessionIdStart != INDEX_NONE)
	{
		SessionIdStart += 12;
		int32 SessionIdEnd = ResultText.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SessionIdStart);
		SessionId = ResultText.Mid(SessionIdStart, SessionIdEnd - SessionIdStart);
	}

	// Test import_nodes with T3D
	{
		FString T3DText = TEXT(
			"Begin Object Class=/Script/BlueprintGraph.K2Node_CallFunction Name=\"K2Node_CallFunction_0\"\n"
			"   FunctionReference=(MemberParent=/Script/CoreUObject.Class'\"/Script/Engine.KismetSystemLibrary\"',MemberName=\"PrintString\")\n"
			"   NodePosX=500\n"
			"   NodePosY=100\n"
			"   NodeGuid=AAAAAAAA111111112222222233333333\n"
			"   CustomProperties Pin (PinId=1111111111111111111111111111111A,PinName=\"execute\",PinType.PinCategory=\"exec\",Direction=\"EGPD_Input\")\n"
			"   CustomProperties Pin (PinId=1111111111111111111111111111111B,PinName=\"then\",Direction=\"EGPD_Output\",PinType.PinCategory=\"exec\")\n"
			"   CustomProperties Pin (PinId=1111111111111111111111111111111C,PinName=\"InString\",PinType.PinCategory=\"string\",DefaultValue=\"Imported from T3D!\")\n"
			"End Object"
		);

		TSharedPtr<FJsonObject> ImportArgs = MakeShared<FJsonObject>();
		ImportArgs->SetStringField(TEXT("operation"), TEXT("import_nodes"));
		ImportArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> ImportParams = MakeShared<FJsonObject>();
		ImportParams->SetStringField(TEXT("t3d_text"), T3DText);
		ImportArgs->SetObjectField(TEXT("params"), ImportParams);

		Result = Tool.Execute(ImportArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to import nodes: %s"), *Result.GetContentAsString()));
			return false;
		}

		AddInfo(TEXT("✓ Successfully imported nodes from T3D"));
	}

	// Compile and save
	{
		TSharedPtr<FJsonObject> CompileArgs = MakeShared<FJsonObject>();
		CompileArgs->SetStringField(TEXT("operation"), TEXT("compile"));
		CompileArgs->SetStringField(TEXT("session_id"), SessionId);
		CompileArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Tool.Execute(CompileArgs);

		TSharedPtr<FJsonObject> SaveArgs = MakeShared<FJsonObject>();
		SaveArgs->SetStringField(TEXT("operation"), TEXT("save"));
		SaveArgs->SetStringField(TEXT("session_id"), SessionId);
		SaveArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Tool.Execute(SaveArgs);
	}

	// Close session
	TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
	CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
	CloseArgs->SetStringField(TEXT("session_id"), SessionId);
	CloseArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
	Tool.Execute(CloseArgs);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditBlueprintGraphTest_ErrorHandling,
	"Claireon.EditBlueprintGraph.ErrorHandling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditBlueprintGraphTest_ErrorHandling::RunTest(const FString& Parameters)
{
	ClaireonTool_EditBlueprintGraph Tool;

	// Test 1: Invalid operation
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("invalid_operation"));
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		auto Result = Tool.Execute(Args);
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for invalid operation"));
			return false;
		}
		AddInfo(TEXT("✓ Correctly rejected invalid operation"));
	}

	// Test 2: Missing session_id
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("operation"), TEXT("add_node"));
		Args->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());

		auto Result = Tool.Execute(Args);
		if (!Result.bIsError || !Result.GetContentAsString().Contains(TEXT("session_id")))
		{
			AddError(TEXT("Expected error about missing session_id"));
			return false;
		}
		AddInfo(TEXT("✓ Correctly rejected missing session_id"));
	}

	// Test 3: Unsupported node type
	{
		// First create a blueprint to get a valid session
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("operation"), TEXT("create"));

		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_ErrorTest"));
		CreateParams->SetObjectField(TEXT("params"), Params);

		auto CreateResult = Tool.Execute(CreateParams);
		if (CreateResult.bIsError)
		{
			AddError(TEXT("Failed to create test blueprint"));
			return false;
		}

		// Extract session ID
		FString ResultText = CreateResult.GetContentAsString();
		FString SessionId;
		int32 SessionIdStart = ResultText.Find(TEXT("Session ID: "));
		if (SessionIdStart != INDEX_NONE)
		{
			SessionIdStart += 12;
			int32 SessionIdEnd = ResultText.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SessionIdStart);
			SessionId = ResultText.Mid(SessionIdStart, SessionIdEnd - SessionIdStart);
		}

		// Try unsupported node type
		TSharedPtr<FJsonObject> AddNodeArgs = MakeShared<FJsonObject>();
		AddNodeArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddNodeArgs->SetStringField(TEXT("session_id"), SessionId);

		TSharedPtr<FJsonObject> NodeParams = MakeShared<FJsonObject>();
		NodeParams->SetStringField(TEXT("node_type"), TEXT("InvalidNodeType"));
		AddNodeArgs->SetObjectField(TEXT("params"), NodeParams);

		auto Result = Tool.Execute(AddNodeArgs);
		if (!Result.bIsError || !Result.GetContentAsString().Contains(TEXT("Unsupported node type")))
		{
			AddError(TEXT("Expected error for unsupported node type"));
			return false;
		}
		AddInfo(TEXT("✓ Correctly rejected unsupported node type with helpful error message"));

		// Close session
		TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
		CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
		CloseArgs->SetStringField(TEXT("session_id"), SessionId);
		CloseArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Tool.Execute(CloseArgs);
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditBlueprintGraphTest_ListGraphs,
	"Claireon.EditBlueprintGraph.ListGraphs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditBlueprintGraphTest_ListGraphs::RunTest(const FString& Parameters)
{
	ClaireonTool_EditBlueprintGraph Tool;

	// Use the blueprint created by FEditBlueprintGraphTest_CreateAndBasicOps
	// (or any known blueprint with at least one graph)
	// Try to open the test blueprint first to ensure it exists
	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("operation"), TEXT("create"));
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_ListGraphsTest"));
		P->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		CreateParams->SetObjectField(TEXT("params"), P);
		Tool.Execute(CreateParams); // Ignore error if already exists
	}

	TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
	Params->SetStringField(TEXT("operation"), TEXT("list_graphs"));
	Params->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_ListGraphsTest"));

	auto Result = Tool.Execute(Params);
	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("list_graphs failed: %s"), *Result.GetContentAsString()));
		return false;
	}

	FString ResultText = Result.GetContentAsString();

	// Must mention the asset path
	if (!ResultText.Contains(TEXT("BP_ListGraphsTest")))
	{
		AddError(TEXT("Result missing asset path"));
		return false;
	}

	// Must list at least one graph (new Actor blueprints always have EventGraph/Ubergraph)
	if (!ResultText.Contains(TEXT("Ubergraph")) && !ResultText.Contains(TEXT("EventGraph")))
	{
		AddError(FString::Printf(TEXT("Expected at least one Ubergraph, got: %s"), *ResultText));
		return false;
	}

	AddInfo(FString::Printf(TEXT("list_graphs result:\n%s"), *ResultText));

	// Error case: invalid path
	{
		TSharedPtr<FJsonObject> ErrParams = MakeShared<FJsonObject>();
		ErrParams->SetStringField(TEXT("operation"), TEXT("list_graphs"));
		ErrParams->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_DoesNotExist_XYZ"));

		auto ErrResult = Tool.Execute(ErrParams);
		if (!ErrResult.bIsError)
		{
			AddError(TEXT("Expected error for non-existent blueprint"));
			return false;
		}
		AddInfo(TEXT("Correctly returned error for non-existent blueprint"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditBlueprintGraphTest_StatelessNodeOps,
	"Claireon.EditBlueprintGraph.StatelessNodeOps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditBlueprintGraphTest_StatelessNodeOps::RunTest(const FString& Parameters)
{
	ClaireonTool_EditBlueprintGraph Tool;

	// Create a blueprint and add a node, then close the session to test stateless ops
	FString SessionId;
	FString NodeGuid;

	{
		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("operation"), TEXT("create"));
		TSharedPtr<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_StatelessOpsTest"));
		P->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		CreateParams->SetObjectField(TEXT("params"), P);

		auto Result = Tool.Execute(CreateParams);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to create blueprint: %s"), *Result.GetContentAsString()));
			return false;
		}

		// Extract session ID
		FString ResultText = Result.GetContentAsString();
		int32 SessionIdStart = ResultText.Find(TEXT("Session ID: "));
		if (SessionIdStart != INDEX_NONE)
		{
			SessionIdStart += 12;
			int32 End = ResultText.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, SessionIdStart);
			SessionId = ResultText.Mid(SessionIdStart, End - SessionIdStart);
		}

		// Add a Branch node
		TSharedPtr<FJsonObject> AddArgs = MakeShared<FJsonObject>();
		AddArgs->SetStringField(TEXT("operation"), TEXT("add_node"));
		AddArgs->SetStringField(TEXT("session_id"), SessionId);
		TSharedPtr<FJsonObject> NP = MakeShared<FJsonObject>();
		NP->SetStringField(TEXT("node_type"), TEXT("Branch"));
		AddArgs->SetObjectField(TEXT("params"), NP);

		Result = Tool.Execute(AddArgs);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add Branch node: %s"), *Result.GetContentAsString()));
			return false;
		}

		// Extract the node GUID from the result (cursor state shows focused node GUID)
		ResultText = Result.GetContentAsString();
		int32 GuidStart = ResultText.Find(TEXT("Focused Node: "));
		if (GuidStart != INDEX_NONE)
		{
			GuidStart += 14;
			int32 GuidEnd = ResultText.Find(TEXT("\n"), ESearchCase::IgnoreCase, ESearchDir::FromStart, GuidStart);
			NodeGuid = ResultText.Mid(GuidStart, GuidEnd - GuidStart).TrimStartAndEnd();
		}

		// Save and close session
		TSharedPtr<FJsonObject> SaveArgs = MakeShared<FJsonObject>();
		SaveArgs->SetStringField(TEXT("operation"), TEXT("save"));
		SaveArgs->SetStringField(TEXT("session_id"), SessionId);
		SaveArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Tool.Execute(SaveArgs);

		TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
		CloseArgs->SetStringField(TEXT("operation"), TEXT("close"));
		CloseArgs->SetStringField(TEXT("session_id"), SessionId);
		CloseArgs->SetObjectField(TEXT("params"), MakeShared<FJsonObject>());
		Tool.Execute(CloseArgs);
	}

	if (NodeGuid.IsEmpty())
	{
		AddWarning(TEXT("Could not extract node GUID from session state — skipping stateless remove test"));
		return true;
	}

	// Test stateless remove_node
	{
		TSharedPtr<FJsonObject> RemoveParams = MakeShared<FJsonObject>();
		RemoveParams->SetStringField(TEXT("operation"), TEXT("remove_node"));
		RemoveParams->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_StatelessOpsTest"));
		RemoveParams->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		RemoveParams->SetStringField(TEXT("node_guid"), NodeGuid);

		auto Result = Tool.Execute(RemoveParams);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Stateless remove_node failed: %s"), *Result.GetContentAsString()));
			return false;
		}

		FString ResultText = Result.GetContentAsString();
		if (!ResultText.Contains(TEXT("Removed")))
		{
			AddError(FString::Printf(TEXT("Expected 'Removed' in result, got: %s"), *ResultText));
			return false;
		}
		AddInfo(FString::Printf(TEXT("Stateless remove_node succeeded: %s"), *ResultText));
	}

	// Test stateless reconstruct_node with an invalid GUID (error case)
	{
		TSharedPtr<FJsonObject> ReconParams = MakeShared<FJsonObject>();
		ReconParams->SetStringField(TEXT("operation"), TEXT("reconstruct_node"));
		ReconParams->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_StatelessOpsTest"));
		ReconParams->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		ReconParams->SetStringField(TEXT("node_guid"), TEXT("FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF"));

		auto Result = Tool.Execute(ReconParams);
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for non-existent node GUID in reconstruct_node"));
			return false;
		}
		AddInfo(TEXT("reconstruct_node correctly returned error for invalid GUID"));
	}

	// Test stateless remove_node missing required fields
	{
		TSharedPtr<FJsonObject> BadParams = MakeShared<FJsonObject>();
		BadParams->SetStringField(TEXT("operation"), TEXT("remove_node"));
		// No asset_path, graph_name, or node_guid

		auto Result = Tool.Execute(BadParams);
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for missing required fields in stateless remove_node"));
			return false;
		}
		AddInfo(TEXT("Correctly returned error for missing fields in stateless remove_node"));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditBlueprintGraphTest_SetGameplayTags,
	"Claireon.EditBlueprintGraph.SetGameplayTags",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditBlueprintGraphTest_SetGameplayTags::RunTest(const FString& Parameters)
{
	ClaireonTool_EditBlueprintGraph Tool;

	// Test: missing asset_path
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("operation"), TEXT("set_gameplay_tags"));
		Params->SetStringField(TEXT("property_path"), TEXT("SomeProperty"));

		auto Result = Tool.Execute(Params);
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for missing asset_path"));
			return false;
		}
		AddInfo(TEXT("Correctly rejected missing asset_path"));
	}

	// Test: missing property_path
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("operation"), TEXT("set_gameplay_tags"));
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_TestActor"));

		auto Result = Tool.Execute(Params);
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for missing property_path"));
			return false;
		}
		AddInfo(TEXT("Correctly rejected missing property_path"));
	}

	// Test: empty tags_to_add and tags_to_remove
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("operation"), TEXT("set_gameplay_tags"));
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_TestActor"));
		Params->SetStringField(TEXT("property_path"), TEXT("SomeProperty"));
		TArray<TSharedPtr<FJsonValue>> EmptyArray;
		Params->SetArrayField(TEXT("tags_to_add"), EmptyArray);
		Params->SetArrayField(TEXT("tags_to_remove"), EmptyArray);

		auto Result = Tool.Execute(Params);
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for empty tags_to_add and tags_to_remove"));
			return false;
		}
		AddInfo(TEXT("Correctly rejected empty tag arrays"));
	}

	// Test: non-existent asset
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("operation"), TEXT("set_gameplay_tags"));
		Params->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_NonExistent_XYZ"));
		Params->SetStringField(TEXT("property_path"), TEXT("SomeProperty"));

		TArray<TSharedPtr<FJsonValue>> AddTags;
		AddTags.Add(MakeShared<FJsonValueString>(TEXT("Test.Tag")));
		Params->SetArrayField(TEXT("tags_to_add"), AddTags);

		auto Result = Tool.Execute(Params);
		if (!Result.bIsError)
		{
			AddError(TEXT("Expected error for non-existent asset"));
			return false;
		}
		AddInfo(TEXT("Correctly returned error for non-existent asset"));
	}

	AddInfo(TEXT("set_gameplay_tags error handling tests passed"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FEditBlueprintGraphTest_CompileRemoveUnused,
	"Claireon.BlueprintCompile.RemoveUnused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FEditBlueprintGraphTest_CompileRemoveUnused::RunTest(const FString& Parameters)
{
	ClaireonTool_BlueprintCompile CompileTool;

	// Test 1: remove_unused defaults to false (no regression)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("contentPath"), TEXT("/Game/__MCPTests"));

		auto Result = CompileTool.Execute(Args);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Compile without remove_unused failed: %s"), *Result.GetContentAsString()));
			return false;
		}

		FString ResultText = Result.GetContentAsString();
		// Should NOT contain "Remove Unused" when not set
		if (ResultText.Contains(TEXT("Remove Unused")))
		{
			AddError(TEXT("'Remove Unused' output should not appear when remove_unused is not set"));
			return false;
		}
		AddInfo(TEXT("Compile without remove_unused: no regression"));
	}

	// Test 2: remove_unused: true produces output line
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("contentPath"), TEXT("/Game/__MCPTests"));
		Args->SetBoolField(TEXT("remove_unused"), true);

		auto Result = CompileTool.Execute(Args);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Compile with remove_unused failed: %s"), *Result.GetContentAsString()));
			return false;
		}

		FString ResultText = Result.GetContentAsString();
		if (!ResultText.Contains(TEXT("Remove Unused")))
		{
			AddError(FString::Printf(TEXT("Expected 'Remove Unused' in result, got: %s"), *ResultText));
			return false;
		}
		AddInfo(FString::Printf(TEXT("Compile with remove_unused produced expected output:\n%s"), *ResultText));
	}

	return true;
}
