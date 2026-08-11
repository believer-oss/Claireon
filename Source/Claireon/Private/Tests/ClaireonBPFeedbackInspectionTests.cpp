// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Baseline pinning tests for Workstream C (inspection ergonomics), Stage 001
// of the Claireon BP feedback plan (Work #6704).
//
// This file also carries the shared exec-chain + pure-feeder fixture used by
// C-1/C-2/C-3's anchored-default tests (ws-c-inspection.md), built here so
// Stage 003 can add its tests against the same fixture builder without
// re-deriving it. Stage 001 itself only pins the unanchored-dump baseline:
// with no anchor_node_guid at all, the returned node_count already equals
// Graph->Nodes.Num() regardless of exec_only/include_pure_subgraph -- pure
// nodes are ordinary graph nodes in the unanchored linear scan, not filtered
// by the anchored-BFS expansion flag this workstream flips in Stage 003.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonTool_GetBlueprintGraph.h"
#include "Tools/ClaireonTool_ListBlueprintGraphNodes.h"
#include "Tools/ClaireonBlueprintGraphTool_AddPin.h"
#include "Tools/ClaireonBlueprintGraphTool_ConnectPins.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_DisconnectPin.h"
#include "Tools/ClaireonBlueprintGraphTool_InspectNode.h"
#include "Tools/ClaireonBlueprintGraphTool_MoveNode.h"
#include "Tools/ClaireonBlueprintGraphTool_RecombinePin.h"
#include "Tools/ClaireonBlueprintGraphTool_ReconstructNode.h"
#include "Tools/ClaireonBlueprintGraphTool_RemoveNode.h"
#include "Tools/ClaireonBlueprintGraphTool_RemovePin.h"
#include "Tools/ClaireonBlueprintGraphTool_SelectNode.h"
#include "Tools/ClaireonBlueprintGraphTool_SelectPin.h"
#include "Tools/ClaireonBlueprintGraphTool_SetNodeProperty.h"
#include "Tools/ClaireonBlueprintGraphTool_SetPinValue.h"
#include "Tools/ClaireonBlueprintGraphTool_SplitPin.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonOutputGate.h"
#include "ClaireonSessionManager.h"
#include "ClaireonSettings.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Templates/Function.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonITTestsInternal
{
	static const TCHAR* kIT_EventName = TEXT("IT_EntryEvent");

	struct FITFixture
	{
		UBlueprint* BP = nullptr;
		UEdGraph* EventGraph = nullptr;
		UK2Node_CustomEvent* EntryEvent = nullptr;
		UK2Node_CallFunction* Call1 = nullptr;
		UK2Node_CallFunction* Call2 = nullptr;
		UK2Node_CallFunction* PureAdd = nullptr;
	};

	static void IT_CleanupAsset(const FString& AssetPath)
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

	// Builds the exec-chain + pure-feeder fixture:
	//   IT_EntryEvent --then--> Call1(PrintString) --then--> Call2(PrintString)
	//   PureAdd(Add_DoubleDouble, pure) --ReturnValue--> Call2.InString
	// (the last link is type-mismatched on purpose -- MakeLinkTo is a raw
	// structural link, not schema-validated, and these tests only assert on
	// graph/JSON structure, never compile the fixture Blueprint.)
	// Returns false with OutError set on any failure; never asserts (UNTEST
	// asserts must stay in the coroutine body, not in helpers).
	static bool IT_BuildFixture(const FString& AssetPath, FITFixture& Out, FString& OutError)
	{
		IT_CleanupAsset(AssetPath);

		UPackage* Package = CreatePackage(*AssetPath);
		if (!IsValid(Package))
		{
			OutError = FString::Printf(TEXT("CreatePackage failed for %s"), *AssetPath);
			return false;
		}

		const FString AssetName = FPackageName::GetShortName(AssetPath);
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);
		if (!IsValid(BP))
		{
			OutError = FString::Printf(TEXT("CreateBlueprint failed for %s"), *AssetPath);
			return false;
		}
		FAssetRegistryModule::AssetCreated(BP);

		UEdGraph* EventGraph = nullptr;
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (IsValid(G)) { EventGraph = G; break; }
		}
		if (!IsValid(EventGraph))
		{
			OutError = TEXT("Newly created Blueprint has no EventGraph");
			return false;
		}

		UK2Node_CustomEvent* EntryEvent = NewObject<UK2Node_CustomEvent>(EventGraph);
		EntryEvent->CustomFunctionName = FName(kIT_EventName);
		EventGraph->AddNode(EntryEvent, /*bUserAction=*/false, /*bSelectNewNode=*/false);
		EntryEvent->CreateNewGuid();
		EntryEvent->PostPlacedNewNode();
		EntryEvent->AllocateDefaultPins();
		EntryEvent->NodePosX = 0;
		EntryEvent->NodePosY = 0;

		UK2Node_CallFunction* Call1 = NewObject<UK2Node_CallFunction>(EventGraph);
		Call1->FunctionReference.SetExternalMember(
			GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), UKismetSystemLibrary::StaticClass());
		EventGraph->AddNode(Call1, false, false);
		Call1->CreateNewGuid();
		Call1->PostPlacedNewNode();
		Call1->AllocateDefaultPins();
		Call1->NodePosX = 300;

		UK2Node_CallFunction* Call2 = NewObject<UK2Node_CallFunction>(EventGraph);
		Call2->FunctionReference.SetExternalMember(
			GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), UKismetSystemLibrary::StaticClass());
		EventGraph->AddNode(Call2, false, false);
		Call2->CreateNewGuid();
		Call2->PostPlacedNewNode();
		Call2->AllocateDefaultPins();
		Call2->NodePosX = 600;

		UK2Node_CallFunction* PureAdd = NewObject<UK2Node_CallFunction>(EventGraph);
		PureAdd->FunctionReference.SetExternalMember(
			GET_FUNCTION_NAME_CHECKED(UKismetMathLibrary, Add_DoubleDouble), UKismetMathLibrary::StaticClass());
		EventGraph->AddNode(PureAdd, false, false);
		PureAdd->CreateNewGuid();
		PureAdd->PostPlacedNewNode();
		PureAdd->AllocateDefaultPins();
		PureAdd->NodePosX = 600;
		PureAdd->NodePosY = 300;
		if (!PureAdd->IsNodePure())
		{
			OutError = TEXT("Add_FloatFloat did not allocate as a pure node");
			return false;
		}

		UEdGraphPin* EntryThen = EntryEvent->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* Call1Exec = Call1->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		UEdGraphPin* Call1Then = Call1->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* Call2Exec = Call2->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		UEdGraphPin* Call2InString = Call2->FindPin(FName(TEXT("InString")), EGPD_Input);
		UEdGraphPin* PureAddReturn = PureAdd->FindPin(FName(TEXT("ReturnValue")), EGPD_Output);
		if (!EntryThen || !Call1Exec || !Call1Then || !Call2Exec || !Call2InString || !PureAddReturn)
		{
			OutError = FString::Printf(
				TEXT("Fixture pin lookup failed (then=%d call1exec=%d call1then=%d call2exec=%d call2InString=%d pureReturn=%d)"),
				EntryThen != nullptr, Call1Exec != nullptr, Call1Then != nullptr,
				Call2Exec != nullptr, Call2InString != nullptr, PureAddReturn != nullptr);
			return false;
		}
		EntryThen->MakeLinkTo(Call1Exec);
		Call1Then->MakeLinkTo(Call2Exec);
		PureAddReturn->MakeLinkTo(Call2InString);

		BP->MarkPackageDirty();

		const FString PackageFileName = FPackageName::LongPackageNameToFilename(
			AssetPath, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		const FSavePackageResultStruct SaveResult = UPackage::Save(Package, BP, *PackageFileName, SaveArgs);
		if (!SaveResult.IsSuccessful())
		{
			OutError = FString::Printf(TEXT("UPackage::Save failed for %s"), *AssetPath);
			return false;
		}

		Out.BP = BP;
		Out.EventGraph = EventGraph;
		Out.EntryEvent = EntryEvent;
		Out.Call1 = Call1;
		Out.Call2 = Call2;
		Out.PureAdd = PureAdd;
		return true;
	}

	static TSharedPtr<FJsonObject> IT_FirstGraph(const IClaireonTool::FToolResult& ToolResult)
	{
		if (ToolResult.bIsError || !ToolResult.Data.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* GraphValues = nullptr;
		if (!ToolResult.Data->TryGetArrayField(TEXT("graphs"), GraphValues) || GraphValues->Num() == 0)
		{
			return nullptr;
		}
		return (*GraphValues)[0]->AsObject();
	}
} // namespace ClaireonITTestsInternal

using namespace ClaireonITTestsInternal;

// ============================================================================
// Unanchored dump baseline (pin only): with no anchor_node_guid at all, the
// returned node_count already equals the graph's actual node count. Pure
// nodes are present because they are ordinary graph nodes in the unanchored
// linear scan, not because of the anchored pure-subgraph expansion flag this
// workstream changes the default of in Stage 003.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, UnanchoredDump_Unchanged, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_IT_UnanchoredDump");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);
	UNTEST_ASSERT_PTR(Fixture.EventGraph);
	// A freshly created Actor Blueprint's EventGraph is not necessarily empty
	// (the project's default new-Blueprint template may seed additional
	// nodes/comments); this test does not assume a specific total, only that
	// the four fixture nodes are present among whatever the graph holds.
	UNTEST_EXPECT_TRUE(Fixture.EventGraph->Nodes.Num() >= 4);

	ClaireonTool_GetBlueprintGraph Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	// No anchor_node_guid, no exec_only, no include_pure_subgraph: defaults only.
	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	TSharedPtr<FJsonObject> GraphObj = IT_FirstGraph(Result);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());

	int32 NodeCount = 0;
	int32 TotalNodesInGraph = 0;
	UNTEST_ASSERT_TRUE(GraphObj->TryGetNumberField(TEXT("node_count"), NodeCount));
	UNTEST_ASSERT_TRUE(GraphObj->TryGetNumberField(TEXT("total_nodes_in_graph"), TotalNodesInGraph));
	UNTEST_EXPECT_EQ(NodeCount, TotalNodesInGraph);
	UNTEST_EXPECT_EQ(NodeCount, Fixture.EventGraph->Nodes.Num());

	IT_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// C-2 / B-4: shared >=8-hex GUID-prefix resolver.
//
// Helpers below drive the 14 node_guid consumers (5 shared-base + 9 inline,
// with bp_connect_pins counted as its two endpoints) through their real
// Execute() with a live session, so the conformance sweep proves every
// consumer routes through ClaireonBlueprintHelpers::ResolveNodeGuidString.
// ============================================================================
namespace ClaireonGRTestsInternal
{
	static void GR_Cleanup(const FString& AssetPath)
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

	// Create the asset + open a bp session in one call (Create tool). Empty on failure.
	static FString GR_CreateSession(const TCHAR* AssetPath)
	{
		ClaireonBlueprintGraphTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));
		IClaireonTool::FToolResult R = CreateTool.Execute(Args);
		if (R.bIsError || !R.Data.IsValid()) { return FString(); }
		FString SessionId;
		R.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	static UEdGraph* GR_EventGraph(UBlueprint* BP)
	{
		if (!IsValid(BP)) { return nullptr; }
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (IsValid(G)) { return G; }
		}
		return nullptr;
	}

	static UBlueprint* GR_LoadBP(const TCHAR* AssetPath)
	{
		return Cast<UBlueprint>(FSoftObjectPath(
			FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	}

	// One node_guid consumer: its tool + a Fill(Args, guidValue) that sets the
	// guid on the correct field plus whatever else must be present for node
	// resolution to be REACHED (fields the tool reads before resolving).
	struct FSweepEntry
	{
		TSharedPtr<IClaireonTool> Tool;
		const TCHAR* Label;
		const TCHAR* FieldName; // field carrying the test guid (for the "Invalid <field> format" assertion)
		TFunction<void(FJsonObject&, const FString&)> Fill;
	};
} // namespace ClaireonGRTestsInternal

using namespace ClaireonGRTestsInternal;

// ----------------------------------------------------------------------------
// Conformance sweep: garbage input -> "Invalid <field> format"; ambiguous
// prefix -> ambiguity error naming both candidates. Neither variant mutates
// (resolution fails first), so all 14 consumers share one fixture safely.
// ----------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, GuidResolverSweep_GarbageAndAmbiguous_AllConsumers, UNTEST_TIMEOUTMS(120000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_GR_Sweep");

	GR_Cleanup(AssetPath);
	FString SessionId = GR_CreateSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	UBlueprint* BP = GR_LoadBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* Graph = GR_EventGraph(BP);
	UNTEST_ASSERT_PTR(Graph);
	UNTEST_ASSERT_TRUE(Graph->Nodes.Num() >= 2);

	// Force two nodes to share the 8-hex prefix "ABCDEF01" (distinct full GUIDs).
	Graph->Nodes[0]->NodeGuid = FGuid(0xABCDEF01u, 0x00000001u, 0x00000002u, 0x00000003u);
	Graph->Nodes[1]->NodeGuid = FGuid(0xABCDEF01u, 0x00000004u, 0x00000005u, 0x00000006u);
	const FString ValidSourceGuid = Graph->Nodes[0]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

	TArray<FSweepEntry> Entries;
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_InspectNode>(), TEXT("inspect_node"), TEXT("node_guid"),
		[](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("node_guid"), G); } });
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_MoveNode>(), TEXT("move_node"), TEXT("node_guid"),
		[](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("node_guid"), G); A.SetNumberField(TEXT("position_x"), 0); A.SetNumberField(TEXT("position_y"), 0); } });
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_AddPin>(), TEXT("add_pin"), TEXT("node_guid"),
		[](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("node_guid"), G); } });
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_DisconnectPin>(), TEXT("disconnect_pin"), TEXT("node_guid"),
		[](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("node_guid"), G); A.SetStringField(TEXT("pin_name"), TEXT("then")); } });
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_RecombinePin>(), TEXT("recombine_pin"), TEXT("node_guid"),
		[](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("node_guid"), G); A.SetStringField(TEXT("pin_name"), TEXT("then")); } });
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_ReconstructNode>(), TEXT("reconstruct_node"), TEXT("node_guid"),
		[](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("node_guid"), G); } });
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_RemoveNode>(), TEXT("remove_node"), TEXT("node_guid"),
		[](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("node_guid"), G); } });
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_RemovePin>(), TEXT("remove_pin"), TEXT("node_guid"),
		[](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("node_guid"), G); } });
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_SelectNode>(), TEXT("select_node"), TEXT("node_guid"),
		[](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("node_guid"), G); } });
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_SelectPin>(), TEXT("select_pin"), TEXT("node_guid"),
		[](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("node_guid"), G); A.SetStringField(TEXT("pin_name"), TEXT("then")); } });
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_SetNodeProperty>(), TEXT("set_node_property"), TEXT("node_guid"),
		[](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("node_guid"), G); A.SetStringField(TEXT("property_name"), TEXT("NodePosX")); A.SetStringField(TEXT("property_value"), TEXT("0")); } });
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_SetPinValue>(), TEXT("set_pin_value"), TEXT("node_guid"),
		[](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("node_guid"), G); A.SetStringField(TEXT("pin_name"), TEXT("then")); A.SetStringField(TEXT("value"), TEXT("0")); } });
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_SplitPin>(), TEXT("split_pin"), TEXT("node_guid"),
		[](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("node_guid"), G); A.SetStringField(TEXT("pin_name"), TEXT("then")); } });
	// connect_pins: two distinct call sites. Source-endpoint garbage fails source
	// resolution outright; target-endpoint needs a valid source first.
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_ConnectPins>(), TEXT("connect_pins/source"), TEXT("source_node_guid"),
		[ValidSourceGuid](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("source_node_guid"), G); A.SetStringField(TEXT("source_pin_name"), TEXT("then")); A.SetStringField(TEXT("target_node_guid"), ValidSourceGuid); A.SetStringField(TEXT("target_pin_name"), TEXT("exec")); } });
	Entries.Add({ MakeShared<ClaireonBlueprintGraphTool_ConnectPins>(), TEXT("connect_pins/target"), TEXT("target_node_guid"),
		[ValidSourceGuid](FJsonObject& A, const FString& G){ A.SetStringField(TEXT("source_node_guid"), ValidSourceGuid); A.SetStringField(TEXT("source_pin_name"), TEXT("then")); A.SetStringField(TEXT("target_node_guid"), G); A.SetStringField(TEXT("target_pin_name"), TEXT("exec")); } });

	UNTEST_EXPECT_EQ(Entries.Num(), 15);

	for (FSweepEntry& E : Entries)
	{
		// (d) garbage: not a GUID, under 8 hex chars after hyphen strip.
		{
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("session_id"), SessionId);
			E.Fill(*Args, TEXT("1234"));
			IClaireonTool::FToolResult R = E.Tool->Execute(Args);
			if (!R.bIsError || !R.ErrorMessage.Contains(FString::Printf(TEXT("Invalid %s format"), E.FieldName)))
			{
				UE_LOG(LogTemp, Error, TEXT("[GR sweep] %s garbage: bIsError=%d msg='%s'"), E.Label, R.bIsError, *R.ErrorMessage);
			}
			UNTEST_EXPECT_TRUE(R.bIsError);
			UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(FString::Printf(TEXT("Invalid %s format"), E.FieldName)));
			UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("1234")));
		}
		// (c) ambiguous 8-hex prefix shared by two nodes.
		{
			TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
			Args->SetStringField(TEXT("session_id"), SessionId);
			E.Fill(*Args, TEXT("ABCDEF01"));
			IClaireonTool::FToolResult R = E.Tool->Execute(Args);
			if (!R.bIsError || !R.ErrorMessage.Contains(TEXT("ambiguous")))
			{
				UE_LOG(LogTemp, Error, TEXT("[GR sweep] %s ambiguous: bIsError=%d msg='%s'"), E.Label, R.bIsError, *R.ErrorMessage);
			}
			UNTEST_EXPECT_TRUE(R.bIsError);
			UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("ambiguous")));
			// Names both candidate full GUIDs.
			UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(Graph->Nodes[0]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)));
			UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(Graph->Nodes[1]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens)));
		}
	}

	GR_Cleanup(AssetPath);
	co_return;
}

// ----------------------------------------------------------------------------
// Success path, shared-base shape (bp_inspect_node via ResolveTargetNode):
// full GUID and a unique 8-hex prefix both resolve.
// ----------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, GuidResolver_SharedBase_FullAndUniquePrefixResolve, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_GR_SharedBase");

	GR_Cleanup(AssetPath);
	FString SessionId = GR_CreateSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());
	UBlueprint* BP = GR_LoadBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* Graph = GR_EventGraph(BP);
	UNTEST_ASSERT_PTR(Graph);
	UNTEST_ASSERT_TRUE(Graph->Nodes.Num() >= 1);

	Graph->Nodes[0]->NodeGuid = FGuid(0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u);
	const FString FullGuid = Graph->Nodes[0]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

	ClaireonBlueprintGraphTool_InspectNode Tool;
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_guid"), FullGuid);
		IClaireonTool::FToolResult R = Tool.Execute(Args);
		UNTEST_EXPECT_FALSE(R.bIsError);
	}
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_guid"), TEXT("11111111")); // unique 8-hex prefix
		IClaireonTool::FToolResult R = Tool.Execute(Args);
		UNTEST_EXPECT_FALSE(R.bIsError);
	}

	GR_Cleanup(AssetPath);
	co_return;
}

// ----------------------------------------------------------------------------
// Success path, inline-parser shape (bp_select_node via FindNodeForOperationStr):
// full GUID and a unique 8-hex prefix both resolve.
// ----------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, GuidResolver_Inline_FullAndUniquePrefixResolve, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_GR_Inline");

	GR_Cleanup(AssetPath);
	FString SessionId = GR_CreateSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());
	UBlueprint* BP = GR_LoadBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* Graph = GR_EventGraph(BP);
	UNTEST_ASSERT_PTR(Graph);
	UNTEST_ASSERT_TRUE(Graph->Nodes.Num() >= 1);

	Graph->Nodes[0]->NodeGuid = FGuid(0x1234ABCDu, 0x22222222u, 0x33333333u, 0x44444444u);
	const FString FullGuid = Graph->Nodes[0]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

	ClaireonBlueprintGraphTool_SelectNode Tool;
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_guid"), FullGuid);
		IClaireonTool::FToolResult R = Tool.Execute(Args);
		UNTEST_EXPECT_FALSE(R.bIsError);
	}
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("node_guid"), TEXT("1234ABCD")); // unique 8-hex prefix
		IClaireonTool::FToolResult R = Tool.Execute(Args);
		UNTEST_EXPECT_FALSE(R.bIsError);
	}

	GR_Cleanup(AssetPath);
	co_return;
}

// ----------------------------------------------------------------------------
// GuidCorrections bookkeeping must still fire for the full-GUID A-field
// recompile-recovery case after the migration (regression for the gap ws-c
// flagged). A stale full GUID (same .A, different .B/.C/.D) resolves via the
// A-field fallback and the response carries the "GUID Corrections" note.
// ----------------------------------------------------------------------------
UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, GuidCorrectionsStillFireOnAFieldRecovery, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_GR_Corrections");

	GR_Cleanup(AssetPath);
	FString SessionId = GR_CreateSession(AssetPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());
	UBlueprint* BP = GR_LoadBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* Graph = GR_EventGraph(BP);
	UNTEST_ASSERT_PTR(Graph);
	UNTEST_ASSERT_TRUE(Graph->Nodes.Num() >= 1);

	// Distinctive A field so exactly one node matches the A-field fallback.
	Graph->Nodes[0]->NodeGuid = FGuid(0x0BADF00Du, 0x00001111u, 0x00002222u, 0x00003333u);
	const FString StaleGuid = Graph->Nodes[0]->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
	// Simulate a recompile that regenerated B/C/D but preserved A.
	Graph->Nodes[0]->NodeGuid = FGuid(0x0BADF00Du, 0x00004444u, 0x00005555u, 0x00006666u);

	ClaireonBlueprintGraphTool_SelectNode Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("node_guid"), StaleGuid);
	IClaireonTool::FToolResult R = Tool.Execute(Args);

	UNTEST_EXPECT_FALSE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("GUID Corrections")));

	GR_Cleanup(AssetPath);
	co_return;
}

// ============================================================================
// B-4: bp_get_graph anchor failures are structured errors, never a silent
// node_count=0. Reuses the C-1 exec-chain fixture (IT_BuildFixture).
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, AnchorGarbage_StructuredError, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_GR_AnchorGarbage");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);

	ClaireonTool_GetBlueprintGraph Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	Args->SetStringField(TEXT("anchor_node_guid"), TEXT("1234"));
	IClaireonTool::FToolResult Result = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("anchor_node_guid")));
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("1234")));

	IT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, AnchorValidButAbsent_StructuredError, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_GR_AnchorAbsent");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);

	// Well-formed GUID that is not present in the graph (and whose .A field
	// matches no node, so the A-field fallback cannot recover it either).
	static const TCHAR* AbsentGuid = TEXT("AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE");

	ClaireonTool_GetBlueprintGraph Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	Args->SetStringField(TEXT("anchor_node_guid"), AbsentGuid);
	IClaireonTool::FToolResult Result = Tool.Execute(Args);

	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("not found")));

	IT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, AnchorUniquePrefix_ResolvesSubgraph, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_GR_AnchorPrefix");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);
	UNTEST_ASSERT_PTR(Fixture.EntryEvent);

	// 8-hex prefix of the entry event's GUID (unique among the fixture nodes).
	const FString AnchorPrefix = Fixture.EntryEvent->NodeGuid.ToString(EGuidFormats::Digits).Left(8);

	ClaireonTool_GetBlueprintGraph Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	Args->SetStringField(TEXT("anchor_node_guid"), AnchorPrefix);
	IClaireonTool::FToolResult Result = Tool.Execute(Args);

	UNTEST_EXPECT_FALSE(Result.bIsError);
	TSharedPtr<FJsonObject> GraphObj = IT_FirstGraph(Result);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());
	int32 NodeCount = 0;
	UNTEST_ASSERT_TRUE(GraphObj->TryGetNumberField(TEXT("node_count"), NodeCount));
	UNTEST_EXPECT_TRUE(NodeCount >= 1);

	IT_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// C-1 (Stage 003): anchored pure-subgraph default flip + exec_only.
// Precedence under test: exec_only=true > explicit include_pure_subgraph >
// anchored default true > false.
// ============================================================================
namespace ClaireonC13TestsInternal
{
	/** node_id strings from a graph object's nodes[], in payload order. */
	static TArray<FString> C13_NodeIds(const TSharedPtr<FJsonObject>& GraphObj)
	{
		TArray<FString> Ids;
		if (!GraphObj.IsValid())
		{
			return Ids;
		}
		const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
		if (!GraphObj->TryGetArrayField(TEXT("nodes"), NodeValues) || !NodeValues)
		{
			return Ids;
		}
		for (const TSharedPtr<FJsonValue>& V : *NodeValues)
		{
			TSharedPtr<FJsonObject> NodeObj = V.IsValid() ? V->AsObject() : nullptr;
			FString Id;
			if (NodeObj.IsValid() && NodeObj->TryGetStringField(TEXT("node_id"), Id))
			{
				Ids.Add(Id);
			}
		}
		return Ids;
	}

	static bool C13_Contains(const TArray<FString>& Ids, const UEdGraphNode* Node)
	{
		return IsValid(Node) && Ids.Contains(Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
	}

	/** Anchored get_graph at the fixture entry event, with caller-supplied extra args. */
	static IClaireonTool::FToolResult C13_AnchoredCall(
		const FString& AssetPath, const FITFixture& Fixture,
		TFunctionRef<void(TSharedPtr<FJsonObject>&)> Configure)
	{
		ClaireonTool_GetBlueprintGraph Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		Args->SetStringField(TEXT("anchor_node_guid"),
			Fixture.EntryEvent->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens));
		Args->SetStringField(TEXT("node_detail_level"), TEXT("full"));
		Configure(Args);
		return Tool.Execute(Args);
	}
} // namespace ClaireonC13TestsInternal

using namespace ClaireonC13TestsInternal;

UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, AnchoredDefaults_IncludesPureFeeders, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_C1_AnchoredDefaults");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);
	UNTEST_ASSERT_PTR(Fixture.PureAdd);

	// No exec_only, no include_pure_subgraph: the flipped anchored default applies.
	IClaireonTool::FToolResult Result = C13_AnchoredCall(AssetPath, Fixture,
		[](TSharedPtr<FJsonObject>&) {});
	UNTEST_ASSERT_FALSE(Result.bIsError);

	TSharedPtr<FJsonObject> GraphObj = IT_FirstGraph(Result);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());
	const TArray<FString> Ids = C13_NodeIds(GraphObj);

	// Exec chain reachable from the anchor, plus the pure feeder into Call2.
	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.EntryEvent));
	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.Call1));
	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.Call2));
	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.PureAdd));

	IT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, AnchoredExecOnlyTrue_ExcludesPureFeeders, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_C1_ExecOnly");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);
	UNTEST_ASSERT_PTR(Fixture.PureAdd);

	IClaireonTool::FToolResult Result = C13_AnchoredCall(AssetPath, Fixture,
		[](TSharedPtr<FJsonObject>& Args) { Args->SetBoolField(TEXT("exec_only"), true); });
	UNTEST_ASSERT_FALSE(Result.bIsError);

	TSharedPtr<FJsonObject> GraphObj = IT_FirstGraph(Result);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());
	const TArray<FString> Ids = C13_NodeIds(GraphObj);

	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.EntryEvent));
	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.Call2));
	UNTEST_EXPECT_FALSE(C13_Contains(Ids, Fixture.PureAdd));

	IT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, AnchoredLegacyIncludePureSubgraphFalse_ExcludesPureFeeders, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_C1_LegacyFalse");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);
	UNTEST_ASSERT_PTR(Fixture.PureAdd);

	// Back-compat: explicit include_pure_subgraph=false still suppresses, with no exec_only.
	IClaireonTool::FToolResult Result = C13_AnchoredCall(AssetPath, Fixture,
		[](TSharedPtr<FJsonObject>& Args) { Args->SetBoolField(TEXT("include_pure_subgraph"), false); });
	UNTEST_ASSERT_FALSE(Result.bIsError);

	TSharedPtr<FJsonObject> GraphObj = IT_FirstGraph(Result);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());
	const TArray<FString> Ids = C13_NodeIds(GraphObj);

	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.EntryEvent));
	UNTEST_EXPECT_FALSE(C13_Contains(Ids, Fixture.PureAdd));

	IT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, AnchoredConflicting_ExecOnlyWins, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_C1_Conflicting");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);
	UNTEST_ASSERT_PTR(Fixture.PureAdd);

	// exec_only wins outright over an explicit include_pure_subgraph=true.
	IClaireonTool::FToolResult Result = C13_AnchoredCall(AssetPath, Fixture,
		[](TSharedPtr<FJsonObject>& Args)
		{
			Args->SetBoolField(TEXT("exec_only"), true);
			Args->SetBoolField(TEXT("include_pure_subgraph"), true);
		});
	UNTEST_ASSERT_FALSE(Result.bIsError);

	TSharedPtr<FJsonObject> GraphObj = IT_FirstGraph(Result);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());
	const TArray<FString> Ids = C13_NodeIds(GraphObj);

	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.EntryEvent));
	UNTEST_EXPECT_FALSE(C13_Contains(Ids, Fixture.PureAdd));

	IT_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// C-3 (Stage 003): server-side filter_class / filter_title_contains / offset,
// plus the total_filtered response field.
//
// Expected counts are computed from the live graph rather than pinned to
// literals: a freshly created Actor Blueprint's EventGraph carries template
// nodes whose exact set is a project setting, not a contract.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, FilterClass_CallFunctionOnly, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_C3_FilterClass");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);
	UNTEST_ASSERT_PTR(Fixture.EventGraph);

	int32 LiveCallFunctionCount = 0;
	for (UEdGraphNode* N : Fixture.EventGraph->Nodes)
	{
		if (IsValid(N) && N->IsA<UK2Node_CallFunction>()) { ++LiveCallFunctionCount; }
	}
	UNTEST_ASSERT_TRUE(LiveCallFunctionCount >= 3);

	ClaireonTool_GetBlueprintGraph Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	Args->SetStringField(TEXT("filter_class"), TEXT("K2Node_CallFunction"));
	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	TSharedPtr<FJsonObject> GraphObj = IT_FirstGraph(Result);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());

	int32 NodeCount = 0;
	int32 TotalFiltered = 0;
	UNTEST_ASSERT_TRUE(GraphObj->TryGetNumberField(TEXT("node_count"), NodeCount));
	UNTEST_ASSERT_TRUE(GraphObj->TryGetNumberField(TEXT("total_filtered"), TotalFiltered));
	UNTEST_EXPECT_EQ(TotalFiltered, LiveCallFunctionCount);
	UNTEST_EXPECT_EQ(NodeCount, LiveCallFunctionCount);

	const TArray<FString> Ids = C13_NodeIds(GraphObj);
	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.Call1));
	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.Call2));
	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.PureAdd));
	// The custom event is not a CallFunction and must be filtered out.
	UNTEST_EXPECT_FALSE(C13_Contains(Ids, Fixture.EntryEvent));

	IT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, FilterClass_Subclass, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_C3_FilterSubclass");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);
	UNTEST_ASSERT_PTR(Fixture.EventGraph);

	// Filtering by the K2Node base must match every K2Node subclass, not just
	// nodes whose class name is literally "K2Node".
	int32 LiveK2NodeCount = 0;
	for (UEdGraphNode* N : Fixture.EventGraph->Nodes)
	{
		if (IsValid(N) && N->IsA<UK2Node>()) { ++LiveK2NodeCount; }
	}
	UNTEST_ASSERT_TRUE(LiveK2NodeCount >= 4);

	ClaireonTool_GetBlueprintGraph Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	Args->SetStringField(TEXT("filter_class"), TEXT("K2Node"));
	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	TSharedPtr<FJsonObject> GraphObj = IT_FirstGraph(Result);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());

	int32 TotalFiltered = 0;
	UNTEST_ASSERT_TRUE(GraphObj->TryGetNumberField(TEXT("total_filtered"), TotalFiltered));
	UNTEST_EXPECT_EQ(TotalFiltered, LiveK2NodeCount);

	const TArray<FString> Ids = C13_NodeIds(GraphObj);
	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.EntryEvent));
	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.Call1));
	UNTEST_EXPECT_TRUE(C13_Contains(Ids, Fixture.PureAdd));

	IT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, FilterClass_Unresolvable, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_C3_FilterBadClass");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);

	ClaireonTool_GetBlueprintGraph Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	Args->SetStringField(TEXT("filter_class"), TEXT("ThisClassDoesNotExist_XYZ"));
	IClaireonTool::FToolResult Result = Tool.Execute(Args);

	// Unresolvable filter is fatal, not silently ignored, and the resolver's own
	// text is surfaced rather than a generic stand-in.
	UNTEST_EXPECT_TRUE(Result.bIsError);
	UNTEST_EXPECT_FALSE(Result.ErrorMessage.IsEmpty());
	UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("ThisClassDoesNotExist_XYZ")));

	IT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, FilterTitleContains_SingleMatchAndCaseInsensitive, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_C3_FilterTitle");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);
	UNTEST_ASSERT_PTR(Fixture.EntryEvent);

	// The custom event's name is unique in the fixture and appears in its title.
	auto RunTitleFilter = [](const TCHAR* Needle) -> IClaireonTool::FToolResult
	{
		ClaireonTool_GetBlueprintGraph Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_C3_FilterTitle"));
		Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		Args->SetStringField(TEXT("filter_title_contains"), Needle);
		return Tool.Execute(Args);
	};

	IClaireonTool::FToolResult Exact = RunTitleFilter(kIT_EventName);
	UNTEST_ASSERT_FALSE(Exact.bIsError);
	TSharedPtr<FJsonObject> ExactGraph = IT_FirstGraph(Exact);
	UNTEST_ASSERT_TRUE(ExactGraph.IsValid());
	const TArray<FString> ExactIds = C13_NodeIds(ExactGraph);
	UNTEST_EXPECT_EQ(ExactIds.Num(), 1);
	UNTEST_EXPECT_TRUE(C13_Contains(ExactIds, Fixture.EntryEvent));

	int32 ExactTotalFiltered = 0;
	UNTEST_ASSERT_TRUE(ExactGraph->TryGetNumberField(TEXT("total_filtered"), ExactTotalFiltered));
	UNTEST_EXPECT_EQ(ExactTotalFiltered, 1);

	// Same substring, different case -> same single match.
	IClaireonTool::FToolResult Lower = RunTitleFilter(TEXT("it_entryevent"));
	UNTEST_ASSERT_FALSE(Lower.bIsError);
	TSharedPtr<FJsonObject> LowerGraph = IT_FirstGraph(Lower);
	UNTEST_ASSERT_TRUE(LowerGraph.IsValid());
	const TArray<FString> LowerIds = C13_NodeIds(LowerGraph);
	UNTEST_EXPECT_EQ(LowerIds.Num(), 1);
	UNTEST_EXPECT_TRUE(C13_Contains(LowerIds, Fixture.EntryEvent));

	IT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, OffsetAndMaxNodesWindow_PartitionsFilteredSet, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_C3_Offset");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);
	UNTEST_ASSERT_PTR(Fixture.EventGraph);

	auto RunWindow = [](int32 Offset, int32 MaxNodes) -> IClaireonTool::FToolResult
	{
		ClaireonTool_GetBlueprintGraph Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_C3_Offset"));
		Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		if (MaxNodes > 0) { Args->SetNumberField(TEXT("max_nodes"), MaxNodes); }
		if (Offset > 0) { Args->SetNumberField(TEXT("offset"), Offset); }
		return Tool.Execute(Args);
	};

	// Full unwindowed set establishes the reference order and total.
	IClaireonTool::FToolResult FullResult = RunWindow(0, 0);
	UNTEST_ASSERT_FALSE(FullResult.bIsError);
	TSharedPtr<FJsonObject> FullGraph = IT_FirstGraph(FullResult);
	UNTEST_ASSERT_TRUE(FullGraph.IsValid());
	const TArray<FString> FullIds = C13_NodeIds(FullGraph);
	int32 FullTotalFiltered = 0;
	UNTEST_ASSERT_TRUE(FullGraph->TryGetNumberField(TEXT("total_filtered"), FullTotalFiltered));
	UNTEST_EXPECT_EQ(FullTotalFiltered, FullIds.Num());

	const int32 N = 2;
	UNTEST_ASSERT_TRUE(FullIds.Num() > 2 * N);

	IClaireonTool::FToolResult PageA = RunWindow(0, N);
	IClaireonTool::FToolResult PageB = RunWindow(N, N);
	UNTEST_ASSERT_FALSE(PageA.bIsError);
	UNTEST_ASSERT_FALSE(PageB.bIsError);

	TSharedPtr<FJsonObject> GraphA = IT_FirstGraph(PageA);
	TSharedPtr<FJsonObject> GraphB = IT_FirstGraph(PageB);
	UNTEST_ASSERT_TRUE(GraphA.IsValid());
	UNTEST_ASSERT_TRUE(GraphB.IsValid());

	const TArray<FString> IdsA = C13_NodeIds(GraphA);
	const TArray<FString> IdsB = C13_NodeIds(GraphB);
	UNTEST_EXPECT_EQ(IdsA.Num(), N);
	UNTEST_EXPECT_EQ(IdsB.Num(), N);

	// Disjoint pages whose ordered concatenation is the first 2N of the full set.
	for (const FString& Id : IdsA)
	{
		UNTEST_EXPECT_FALSE(IdsB.Contains(Id));
	}
	TArray<FString> Union = IdsA;
	Union.Append(IdsB);
	UNTEST_ASSERT_EQ(Union.Num(), 2 * N);
	for (int32 I = 0; I < 2 * N; ++I)
	{
		UNTEST_EXPECT_TRUE(Union[I] == FullIds[I]);
	}

	// total_filtered is windowing-invariant.
	int32 TotalA = 0;
	int32 TotalB = 0;
	UNTEST_ASSERT_TRUE(GraphA->TryGetNumberField(TEXT("total_filtered"), TotalA));
	UNTEST_ASSERT_TRUE(GraphB->TryGetNumberField(TEXT("total_filtered"), TotalB));
	UNTEST_EXPECT_EQ(TotalA, FullTotalFiltered);
	UNTEST_EXPECT_EQ(TotalB, FullTotalFiltered);

	IT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, Compose_FilterClassAndNodeFilterAndGraphName, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_C3_Compose");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);

	auto RunCompose = [](bool bClass, bool bEntryPoints) -> TArray<FString>
	{
		ClaireonTool_GetBlueprintGraph Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), TEXT("/Game/__MCPTests/BP_C3_Compose"));
		Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
		if (bClass) { Args->SetStringField(TEXT("filter_class"), TEXT("K2Node")); }
		if (bEntryPoints) { Args->SetStringField(TEXT("node_filter"), TEXT("entry_points")); }
		return C13_NodeIds(IT_FirstGraph(Tool.Execute(Args)));
	};

	// Composition must behave as set intersection: neither filter may be dropped
	// when the other is present.
	const TArray<FString> ClassOnly = RunCompose(true, false);
	const TArray<FString> EntryOnly = RunCompose(false, true);
	const TArray<FString> Both = RunCompose(true, true);

	UNTEST_ASSERT_TRUE(ClassOnly.Num() > 0);
	UNTEST_ASSERT_TRUE(EntryOnly.Num() > 0);

	for (const FString& Id : Both)
	{
		UNTEST_EXPECT_TRUE(ClassOnly.Contains(Id));
		UNTEST_EXPECT_TRUE(EntryOnly.Contains(Id));
	}
	int32 ExpectedIntersection = 0;
	for (const FString& Id : ClassOnly)
	{
		if (EntryOnly.Contains(Id)) { ++ExpectedIntersection; }
	}
	UNTEST_EXPECT_EQ(Both.Num(), ExpectedIntersection);

	// graph_name scoping still holds: every returned node belongs to EventGraph.
	for (const FString& Id : Both)
	{
		bool bFoundInEventGraph = false;
		for (UEdGraphNode* N : Fixture.EventGraph->Nodes)
		{
			if (IsValid(N) && N->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens) == Id)
			{
				bFoundInEventGraph = true;
				break;
			}
		}
		UNTEST_EXPECT_TRUE(bFoundInEventGraph);
	}

	IT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, ListNodesAliasInheritsFiltersAndTotalFiltered, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_C3_AliasInherit");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);
	UNTEST_ASSERT_PTR(Fixture.EventGraph);

	// bp_list_nodes is a pure passthrough alias: the C-3 params must reach
	// get_graph through it with no per-tool wiring. Driving the alias class
	// directly is the only way to prove the passthrough did not rot.
	int32 LiveCallFunctionCount = 0;
	for (UEdGraphNode* N : Fixture.EventGraph->Nodes)
	{
		if (IsValid(N) && N->IsA<UK2Node_CallFunction>()) { ++LiveCallFunctionCount; }
	}
	UNTEST_ASSERT_TRUE(LiveCallFunctionCount >= 3);

	ClaireonTool_ListBlueprintGraphNodes AliasTool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	Args->SetStringField(TEXT("filter_class"), TEXT("K2Node_CallFunction"));
	IClaireonTool::FToolResult Result = AliasTool.Execute(Args);
	UNTEST_ASSERT_FALSE(Result.bIsError);

	TSharedPtr<FJsonObject> GraphObj = IT_FirstGraph(Result);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());

	int32 TotalFiltered = 0;
	UNTEST_ASSERT_TRUE(GraphObj->TryGetNumberField(TEXT("total_filtered"), TotalFiltered));
	UNTEST_EXPECT_EQ(TotalFiltered, LiveCallFunctionCount);

	const TArray<FString> Ids = C13_NodeIds(GraphObj);
	UNTEST_EXPECT_EQ(Ids.Num(), LiveCallFunctionCount);
	UNTEST_EXPECT_FALSE(C13_Contains(Ids, Fixture.EntryEvent));

	// The alias also inherits offset windowing.
	TSharedPtr<FJsonObject> PagedArgs = MakeShared<FJsonObject>();
	PagedArgs->SetStringField(TEXT("asset_path"), AssetPath);
	PagedArgs->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	PagedArgs->SetStringField(TEXT("filter_class"), TEXT("K2Node_CallFunction"));
	PagedArgs->SetNumberField(TEXT("offset"), 1);
	PagedArgs->SetNumberField(TEXT("max_nodes"), 1);
	IClaireonTool::FToolResult Paged = AliasTool.Execute(PagedArgs);
	UNTEST_ASSERT_FALSE(Paged.bIsError);

	TSharedPtr<FJsonObject> PagedGraph = IT_FirstGraph(Paged);
	UNTEST_ASSERT_TRUE(PagedGraph.IsValid());
	const TArray<FString> PagedIds = C13_NodeIds(PagedGraph);
	UNTEST_EXPECT_EQ(PagedIds.Num(), 1);
	// total_filtered stays the full filtered size, not the page size.
	int32 PagedTotalFiltered = 0;
	UNTEST_ASSERT_TRUE(PagedGraph->TryGetNumberField(TEXT("total_filtered"), PagedTotalFiltered));
	UNTEST_EXPECT_EQ(PagedTotalFiltered, LiveCallFunctionCount);
	UNTEST_EXPECT_TRUE(PagedIds[0] == Ids[1]);

	IT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackInspection, SpillStillFiresOnLargeUnfilteredResult, UNTEST_TIMEOUTMS(60000))
{
	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_C3_Spill");

	FITFixture Fixture;
	FString FixtureError;
	const bool bBuilt = IT_BuildFixture(AssetPath, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPFeedbackInspection] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);

	const FString TestRoot = FPaths::ProjectIntermediateDir()
		/ TEXT("ClaireonTests") / TEXT("BPFeedbackSpill")
		/ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	IFileManager::Get().MakeDirectory(*TestRoot, /*Tree*/ true);
	FClaireonOutputGate::SetResultsRootOverrideForTests(TestRoot);

	// Unfiltered, full-detail dump -- the filtering additions must not have moved
	// the spill trigger point for callers who pass no filters at all.
	ClaireonTool_GetBlueprintGraph Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("graph_name"), TEXT("EventGraph"));
	Args->SetStringField(TEXT("node_detail_level"), TEXT("full"));
	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	const bool bExecuteFailed = Result.bIsError;

	// Precondition: the payload must actually exceed the spill threshold, else
	// the assertion below would pass or fail for the wrong reason.
	FString Serialized;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	const bool bSerialized = Result.Data.IsValid()
		&& FJsonSerializer::Serialize(Result.Data.ToSharedRef(), Writer);
	const UClaireonSettings* Settings = UClaireonSettings::Get();
	const int32 Threshold = IsValid(Settings) ? Settings->ResultSpillThresholdBytes : 8192;
	const int32 SerializedLen = Serialized.Len();

	IClaireonTool::FToolResult Routed = FClaireonOutputGate::RouteResult(
		MoveTemp(Result), TEXT("bp_get_graph"), TEXT("test_conv"),
		EClaireonSpillStreamSet::GenericData);
	bool bSpilled = false;
	const bool bHasSpillFlag = Routed.Data.IsValid()
		&& Routed.Data->TryGetBoolField(TEXT("__mcp_spilled__"), bSpilled);

	FClaireonOutputGate::SetResultsRootOverrideForTests(FString());
	IFileManager::Get().DeleteDirectory(*TestRoot, /*bRequireExists*/ false, /*Tree*/ true);

	UNTEST_ASSERT_FALSE(bExecuteFailed);
	UNTEST_ASSERT_TRUE(bSerialized);
	UNTEST_ASSERT_TRUE(SerializedLen > Threshold);
	UNTEST_ASSERT_TRUE(bHasSpillFlag);
	UNTEST_EXPECT_TRUE(bSpilled);

	IT_CleanupAsset(AssetPath);
	co_return;
}

#endif // WITH_UNTESTED
