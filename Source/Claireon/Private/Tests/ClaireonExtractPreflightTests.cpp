// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Headless extraction preflight tests. Live collapse and editor behavior are covered by the editor suite.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonSessionManager.h"
#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/ClaireonBlueprintGraphTool_AddVariable.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_Extract.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"

namespace ClaireonExtractPreflightTestsInternal
{
	// Prefix helpers to avoid unity-build collisions.

	static void EPF_Cleanup(const FString& AssetPath)
	{
		FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);
		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> ToDelete;
			ToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(ToDelete);
		}
	}

	static FString EPF_CreateAndOpen(const TCHAR* AssetPath)
	{
		{
			ClaireonBlueprintGraphTool_Create CreateTool;
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("asset_path"), AssetPath);
			Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
			if (CreateTool.Execute(Args).bIsError)
			{
				return FString();
			}
		}
		ClaireonBlueprintGraphTool_Open OpenTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		IClaireonTool::FToolResult R = OpenTool.Execute(Args);
		if (R.bIsError || !R.Data.IsValid())
		{
			return FString();
		}
		FString SessionId;
		R.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	/** A PrintString call node, returning the guid the tool reports for it. */
	static FString EPF_AddPrint(const FString& SessionId)
	{
		ClaireonBlueprintGraphTool_AddNode Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		Args->SetStringField(TEXT("function_name"), TEXT("PrintString"));
		Args->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));
		IClaireonTool::FToolResult R = Tool.Execute(Args);
		FString Guid;
		if (!R.bIsError && R.Data.IsValid())
		{
			R.Data->TryGetStringField(TEXT("created_node_guid"), Guid);
		}
		return Guid;
	}

	/** The session's live graph, so exec links can be made directly for the fixture. */
	static UEdGraph* EPF_Graph(const FString& SessionId)
	{
		FBlueprintEditToolData* Data = ClaireonBlueprintGraphEditToolBase::FindToolData(SessionId);
		return Data ? Data->Graph.Get() : nullptr;
	}

	static UEdGraphNode* EPF_FindNode(UEdGraph* Graph, const FString& Guid)
	{
		const FString Want = Guid.Replace(TEXT("-"), TEXT("")).ToUpper();
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (IsValid(Node)
				&& Node->NodeGuid.ToString(EGuidFormats::Digits).ToUpper() == Want)
			{
				return Node;
			}
		}
		return nullptr;
	}

	static UEdGraphPin* EPF_Pin(UEdGraphNode* Node, EEdGraphPinDirection Dir)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->Direction == Dir
				&& Pin->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				return Pin;
			}
		}
		return nullptr;
	}

	static void EPF_LinkExec(UEdGraph* Graph, const FString& FromGuid, const FString& ToGuid)
	{
		UEdGraphNode* From = EPF_FindNode(Graph, FromGuid);
		UEdGraphNode* To = EPF_FindNode(Graph, ToGuid);
		if (!IsValid(From) || !IsValid(To))
		{
			return;
		}
		UEdGraphPin* Out = EPF_Pin(From, EGPD_Output);
		UEdGraphPin* In = EPF_Pin(To, EGPD_Input);
		if (Out && In)
		{
			Out->MakeLinkTo(In);
		}
	}

	/** Declare a string member variable, so a VariableGet can drive a data pin. */
	static bool EPF_AddStringVariable(const FString& SessionId, const TCHAR* Name)
	{
		ClaireonBlueprintGraphTool_AddVariable Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("variable_name"), Name);
		Args->SetStringField(TEXT("variable_type"), TEXT("string"));
		return !Tool.Execute(Args).bIsError;
	}

	static FString EPF_AddVariableGet(const FString& SessionId, const TCHAR* VarName)
	{
		ClaireonBlueprintGraphTool_AddNode Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_type"), TEXT("VariableGet"));
		Args->SetStringField(TEXT("variable_name"), VarName);
		IClaireonTool::FToolResult R = Tool.Execute(Args);
		FString Guid;
		if (!R.bIsError && R.Data.IsValid())
		{
			R.Data->TryGetStringField(TEXT("created_node_guid"), Guid);
		}
		return Guid;
	}

	/** Link a non-exec output on From to a non-exec input of matching category on To. */
	static bool EPF_LinkData(UEdGraph* Graph, const FString& FromGuid, const FString& ToGuid)
	{
		UEdGraphNode* From = EPF_FindNode(Graph, FromGuid);
		UEdGraphNode* To = EPF_FindNode(Graph, ToGuid);
		if (!IsValid(From) || !IsValid(To))
		{
			return false;
		}
		for (UEdGraphPin* Out : From->Pins)
		{
			if (!Out || Out->Direction != EGPD_Output
				|| Out->PinType.PinCategory == UEdGraphSchema_K2::PC_Exec)
			{
				continue;
			}
			for (UEdGraphPin* In : To->Pins)
			{
				if (In && In->Direction == EGPD_Input
					&& In->PinType.PinCategory == Out->PinType.PinCategory)
				{
					Out->MakeLinkTo(In);
					return true;
				}
			}
		}
		return false;
	}

	static TArray<TSharedPtr<FJsonValue>> EPF_Guids(const TArray<FString>& Guids)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FString& G : Guids)
		{
			Out.Add(MakeShared<FJsonValueString>(G));
		}
		return Out;
	}

	static IClaireonTool::FToolResult EPF_ExtractEvent(const FString& SessionId,
	                                                   const TArray<FString>& Guids)
	{
		ClaireonBlueprintGraphTool_ExtractEvent Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetArrayField(TEXT("node_guids"), EPF_Guids(Guids));
		return Tool.Execute(Args);
	}

	static int32 EPF_EvidenceCount(const IClaireonTool::FToolResult& Result, const TCHAR* Key)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (Result.Data.IsValid() && Result.Data->TryGetArrayField(Key, Arr) && Arr)
		{
			return Arr->Num();
		}
		return -1;
	}

	static IClaireonTool::FToolResult EPF_ExtractEventNamed(const FString& SessionId,
	                                                        const TArray<FString>& Guids,
	                                                        const TCHAR* NewName)
	{
		ClaireonBlueprintGraphTool_ExtractEvent Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetArrayField(TEXT("node_guids"), EPF_Guids(Guids));
		Args->SetStringField(TEXT("new_name"), NewName);
		return Tool.Execute(Args);
	}

	/** Reaching the capability refusal proves the headless control passed earlier preflights. */
	static bool EPF_ReachedCapabilityGate(const IClaireonTool::FToolResult& Result)
	{
		return Result.bIsError && Result.ErrorMessage.Contains(TEXT("requires an interactive editor"));
	}
}

UNTEST_UNIT_OPTS(Claireon, ExtractPreflight, ExtractPreflight_UnknownGuidIsNamed,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonExtractPreflightTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EPF_UnknownGuid");
	const FString SessionId = EPF_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString Real = EPF_AddPrint(SessionId);
	UNTEST_ASSERT_FALSE(Real.IsEmpty());

	ClaireonBlueprintGraphTool_ExtractFunction Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetArrayField(TEXT("node_guids"),
		EPF_Guids({Real, TEXT("DEADBEEF-0000-0000-0000-000000000000")}));
	IClaireonTool::FToolResult R = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("DEADBEEF")));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("bp_get_graph")));

	EPF_Cleanup(AssetPath);
	co_return;
}

// Accept both hyphenated and compact GUIDs emitted by other tools.
UNTEST_UNIT_OPTS(Claireon, ExtractPreflight, ExtractPreflight_BothGuidSpellingsResolve,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonExtractPreflightTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EPF_GuidSpelling");
	const FString SessionId = EPF_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString Unhyphenated = EPF_AddPrint(SessionId);
	UNTEST_ASSERT_FALSE(Unhyphenated.IsEmpty());
	UNTEST_ASSERT_FALSE(Unhyphenated.Contains(TEXT("-")));

	UEdGraph* Graph = EPF_Graph(SessionId);
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	UEdGraphNode* Node = EPF_FindNode(Graph, Unhyphenated);
	UNTEST_ASSERT_TRUE(IsValid(Node));
	const FString Hyphenated = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
	UNTEST_ASSERT_TRUE(Hyphenated.Contains(TEXT("-")));

	// Both spellings must reach the later no-call-site refusal.
	for (const FString& Spelling : {Unhyphenated, Hyphenated})
	{
		IClaireonTool::FToolResult R = EPF_ExtractEvent(SessionId, {Spelling});
		UNTEST_EXPECT_TRUE(R.bIsError);
		UNTEST_EXPECT_FALSE(R.ErrorMessage.Contains(TEXT("not in graph")));
		UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("no call site")));
	}

	EPF_Cleanup(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ExtractPreflight, ExtractPreflight_NoSelectionArgumentIsRejected,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonExtractPreflightTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EPF_NoSelection");
	const FString SessionId = EPF_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	ClaireonBlueprintGraphTool_ExtractFunction Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	IClaireonTool::FToolResult R = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("node_guids")));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("anchor_node_guid")));

	EPF_Cleanup(AssetPath);
	co_return;
}

// Event extraction must reject outgoing exec edges because custom events do not block their callers.
UNTEST_UNIT_OPTS(Claireon, ExtractPreflight, ExtractPreflight_EventRefusesNonTerminalRegion,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonExtractPreflightTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EPF_NonTerminal");
	const FString SessionId = EPF_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString First = EPF_AddPrint(SessionId);
	const FString Second = EPF_AddPrint(SessionId);
	UNTEST_ASSERT_FALSE(First.IsEmpty());
	UNTEST_ASSERT_FALSE(Second.IsEmpty());

	UEdGraph* Graph = EPF_Graph(SessionId);
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	EPF_LinkExec(Graph, First, Second);

	// Selecting only First leaves an exec edge First -> Second crossing the boundary.
	IClaireonTool::FToolResult R = EPF_ExtractEvent(SessionId, {First});
	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("exec edge")));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("STARTS")));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("bp_extract_function")));
	UNTEST_EXPECT_EQ(EPF_EvidenceCount(R, TEXT("exec_edges_leaving_selection")), 1);

	EPF_Cleanup(AssetPath);
	co_return;
}

// Reject crossing data pins because event extraction does not synthesize boundary parameters.
UNTEST_UNIT_OPTS(Claireon, ExtractPreflight, ExtractPreflight_EventRefusesCrossingDataPin,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonExtractPreflightTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EPF_CrossingData");
	const FString SessionId = EPF_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Provide an exec call site so only the crossing data edge disqualifies the selection.
	const FString Caller = EPF_AddPrint(SessionId);
	const FString Selected = EPF_AddPrint(SessionId);
	UNTEST_ASSERT_FALSE(Caller.IsEmpty());
	UNTEST_ASSERT_FALSE(Selected.IsEmpty());
	UNTEST_ASSERT_TRUE(EPF_AddStringVariable(SessionId, TEXT("Label")));
	const FString Getter = EPF_AddVariableGet(SessionId, TEXT("Label"));
	UNTEST_ASSERT_FALSE(Getter.IsEmpty());

	UEdGraph* Graph = EPF_Graph(SessionId);
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	EPF_LinkExec(Graph, Caller, Selected);

	UNTEST_ASSERT_TRUE(EPF_LinkData(Graph, Getter, Selected));

	IClaireonTool::FToolResult R = EPF_ExtractEvent(SessionId, {Selected});
	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("data pin")));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("bp_extract_function")));
	UNTEST_EXPECT_EQ(EPF_EvidenceCount(R, TEXT("data_pins_crossing_boundary")), 1);
	UNTEST_EXPECT_EQ(EPF_EvidenceCount(R, TEXT("exec_edges_leaving_selection")), 0);

	EPF_Cleanup(AssetPath);
	co_return;
}

// Reject selections with no incoming call site before creating anything.
UNTEST_UNIT_OPTS(Claireon, ExtractPreflight, ExtractPreflight_EventRefusesUnreachableRegion,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonExtractPreflightTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EPF_Unreachable");
	const FString SessionId = EPF_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString Lone = EPF_AddPrint(SessionId);
	UNTEST_ASSERT_FALSE(Lone.IsEmpty());

	UEdGraph* Graph = EPF_Graph(SessionId);
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	const int32 NodesBefore = Graph->Nodes.Num();

	IClaireonTool::FToolResult R = EPF_ExtractEvent(SessionId, {Lone});
	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("no call site")));

	UNTEST_EXPECT_EQ(Graph->Nodes.Num(), NodesBefore);

	EPF_Cleanup(AssetPath);
	co_return;
}

// CollapseNodes uses FocusedGraphEdPtr; require it to match the resolved graph before mutation.
UNTEST_UNIT_OPTS(Claireon, ExtractPreflight, ExtractPreflight_CompositeFocusMismatchIsRefused,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonExtractPreflightTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EPF_CompositeFocus");
	const FString SessionId = EPF_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UEdGraph* Resolved = EPF_Graph(SessionId);
	UNTEST_ASSERT_TRUE(IsValid(Resolved));

	UNTEST_EXPECT_TRUE(
		ClaireonExtractQuiescence::FocusedGraphMismatchRefusal(Resolved, Resolved).IsEmpty());

	// Reject absent focus before opening a transaction.
	{
		const FString Refusal =
			ClaireonExtractQuiescence::FocusedGraphMismatchRefusal(Resolved, nullptr);
		UNTEST_EXPECT_FALSE(Refusal.IsEmpty());
		UNTEST_EXPECT_TRUE(Refusal.Contains(TEXT("no focused graph")));
	}

	// A mismatched-focus refusal must identify both graphs.
	{
		UEdGraph* Other = NewObject<UEdGraph>(GetTransientPackage(), TEXT("EPF_OtherFocusedGraph"));
		UNTEST_ASSERT_TRUE(IsValid(Other));
		const FString Refusal =
			ClaireonExtractQuiescence::FocusedGraphMismatchRefusal(Resolved, Other);
		UNTEST_EXPECT_FALSE(Refusal.IsEmpty());
		UNTEST_EXPECT_TRUE(Refusal.Contains(Other->GetName()));
		UNTEST_EXPECT_TRUE(Refusal.Contains(Resolved->GetName()));

		UNTEST_EXPECT_TRUE(Refusal.Contains(TEXT("no transaction was opened")));
		UNTEST_EXPECT_TRUE(Refusal.Contains(TEXT("no mutating API was called")));
	}

	EPF_Cleanup(AssetPath);
	co_return;
}

// Count distinct entry pins, not inbound links: one event Then cannot drive two independent entries.

UNTEST_UNIT_OPTS(Claireon, ExtractPreflight, ExtractPreflight_EventRefusesMultipleEntryPoints,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonExtractPreflightTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EPF_MultiEntry");
	const FString SessionId = EPF_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// Select two terminal regions with distinct incoming target pins.
	const FString CallerA = EPF_AddPrint(SessionId);
	const FString CallerB = EPF_AddPrint(SessionId);
	const FString RegionX = EPF_AddPrint(SessionId);
	const FString RegionY = EPF_AddPrint(SessionId);
	UNTEST_ASSERT_FALSE(CallerA.IsEmpty());
	UNTEST_ASSERT_FALSE(CallerB.IsEmpty());
	UNTEST_ASSERT_FALSE(RegionX.IsEmpty());
	UNTEST_ASSERT_FALSE(RegionY.IsEmpty());

	UEdGraph* Graph = EPF_Graph(SessionId);
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	EPF_LinkExec(Graph, CallerA, RegionX);
	EPF_LinkExec(Graph, CallerB, RegionY);

	const int32 NodesBefore = Graph->Nodes.Num();

	const IClaireonTool::FToolResult R = EPF_ExtractEvent(SessionId, {RegionX, RegionY});
	UNTEST_ASSERT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("distinct entry point")));

	UNTEST_EXPECT_EQ(EPF_EvidenceCount(R, TEXT("exec_edges_leaving_selection")), 0);
	UNTEST_EXPECT_EQ(EPF_EvidenceCount(R, TEXT("data_pins_crossing_boundary")), 0);

	UNTEST_EXPECT_EQ(EPF_EvidenceCount(R, TEXT("entry_pins")), 2);

	UNTEST_EXPECT_EQ(Graph->Nodes.Num(), NodesBefore);

	EPF_Cleanup(AssetPath);
	co_return;
}

// Multiple callers may share one entry pin.
UNTEST_UNIT_OPTS(Claireon, ExtractPreflight, ExtractPreflight_EventAllowsManyCallersIntoOneEntry,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonExtractPreflightTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EPF_ManyCallers");
	const FString SessionId = EPF_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString CallerA = EPF_AddPrint(SessionId);
	const FString CallerB = EPF_AddPrint(SessionId);
	const FString Region = EPF_AddPrint(SessionId);
	UNTEST_ASSERT_FALSE(CallerA.IsEmpty());
	UNTEST_ASSERT_FALSE(CallerB.IsEmpty());
	UNTEST_ASSERT_FALSE(Region.IsEmpty());

	UEdGraph* Graph = EPF_Graph(SessionId);
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	EPF_LinkExec(Graph, CallerA, Region);
	EPF_LinkExec(Graph, CallerB, Region);

	UEdGraphNode* RegionNode = EPF_FindNode(Graph, Region);
	UNTEST_ASSERT_TRUE(RegionNode != nullptr);
	UEdGraphPin* RegionExecIn = EPF_Pin(RegionNode, EGPD_Input);
	UNTEST_ASSERT_TRUE(RegionExecIn != nullptr);
	UNTEST_ASSERT_EQ(RegionExecIn->LinkedTo.Num(), 2);

	const IClaireonTool::FToolResult R = EPF_ExtractEvent(SessionId, {Region});
	UNTEST_EXPECT_FALSE(R.ErrorMessage.Contains(TEXT("distinct entry point")));
	UNTEST_EXPECT_TRUE(EPF_ReachedCapabilityGate(R));

	EPF_Cleanup(AssetPath);
	co_return;
}

// Explicit event names must pass the shared conflict gate.
// Accept either parent-event or template-override conflict detection for ReceiveBeginPlay.

UNTEST_UNIT_OPTS(Claireon, ExtractPreflight, ExtractPreflight_EventRefusesConflictingNewName,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonExtractPreflightTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EPF_NameConflict");
	const FString SessionId = EPF_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString Caller = EPF_AddPrint(SessionId);
	const FString Region = EPF_AddPrint(SessionId);
	UNTEST_ASSERT_FALSE(Caller.IsEmpty());
	UNTEST_ASSERT_FALSE(Region.IsEmpty());

	UEdGraph* Graph = EPF_Graph(SessionId);
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	EPF_LinkExec(Graph, Caller, Region);

	const int32 NodesBefore = Graph->Nodes.Num();

	const IClaireonTool::FToolResult R =
		EPF_ExtractEventNamed(SessionId, {Region}, TEXT("ReceiveBeginPlay"));
	UNTEST_ASSERT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("ReceiveBeginPlay")));

	UNTEST_EXPECT_FALSE(EPF_ReachedCapabilityGate(R));

	UNTEST_ASSERT_TRUE(R.Data.IsValid());
	FString ConflictKind;
	UNTEST_EXPECT_TRUE(R.Data->TryGetStringField(TEXT("name_conflict_kind"), ConflictKind));
	UNTEST_EXPECT_FALSE(ConflictKind.IsEmpty());
	UNTEST_EXPECT_FALSE(ConflictKind == TEXT("none"));

	UNTEST_EXPECT_EQ(Graph->Nodes.Num(), NodesBefore);

	EPF_Cleanup(AssetPath);
	co_return;
}

// A free name must pass preflight.
UNTEST_UNIT_OPTS(Claireon, ExtractPreflight, ExtractPreflight_EventAcceptsFreeNewName,
	UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonExtractPreflightTestsInternal;
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_EPF_FreeName");
	const FString SessionId = EPF_CreateAndOpen(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString Caller = EPF_AddPrint(SessionId);
	const FString Region = EPF_AddPrint(SessionId);
	UNTEST_ASSERT_FALSE(Caller.IsEmpty());
	UNTEST_ASSERT_FALSE(Region.IsEmpty());

	UEdGraph* Graph = EPF_Graph(SessionId);
	UNTEST_ASSERT_TRUE(IsValid(Graph));
	EPF_LinkExec(Graph, Caller, Region);

	const IClaireonTool::FToolResult R =
		EPF_ExtractEventNamed(SessionId, {Region}, TEXT("EPF_AFreshUntakenEventName"));
	UNTEST_EXPECT_TRUE(EPF_ReachedCapabilityGate(R));
	UNTEST_EXPECT_FALSE(R.ErrorMessage.Contains(TEXT("cannot name the event")));

	EPF_Cleanup(AssetPath);
	co_return;
}

#endif // WITH_UNTESTED
