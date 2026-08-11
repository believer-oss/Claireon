// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// WI-4 regression tests: bp_list_graphs and bp_inspect_node must return a
// structured Data object, not summary-only results. ClaireonBridge's
// BuildResultEnvelope substitutes {} for a null Data object, so any tool that
// returns MakeSuccessResult(nullptr, Summary) reads as data:{} to programmatic
// callers even though the operation succeeded.
//
// Creates throwaway Blueprints under /Game/__MCPTests/, drives the tools
// through their JSON Execute entry, and asserts on the structured Data payload.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonBlueprintGraphTool_InspectNode.h"
#include "Tools/ClaireonBlueprintGraphTool_ListGraphs.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonSessionManager.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_DynamicCast.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonListGraphsDataEnvelopeTestsInternal
{
	static const TCHAR* DataEnvTest_ListGraphsBPPath   = TEXT("/Game/__MCPTests/BP_ListGraphsDataEnvelope");
	static const TCHAR* DataEnvTest_InspectNodeBPPath  = TEXT("/Game/__MCPTests/BP_InspectNodeDataEnvelope");

	void DataEnvTest_CleanupAsset(const FString& AssetPath)
	{
		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> AssetsToDelete;
			AssetsToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
		}
		// Belt-and-suspenders for editor-less runs: ForceDeleteObjects can
		// leave the saved package file behind, so remove it directly so
		// /Game/__MCPTests never accumulates files on disk (same pattern as
		// ClaireonCloseSaveContractTests.cpp).
		const FString PackageFileName = FPackageName::LongPackageNameToFilename(
			AssetPath, FPackageName::GetAssetPackageExtension());
		if (IFileManager::Get().FileExists(*PackageFileName))
		{
			IFileManager::Get().Delete(*PackageFileName, /*RequireExists=*/false, /*EvenReadOnly=*/true);
		}
	}

	UBlueprint* DataEnvTest_CreateActorBP(const FString& AssetPath)
	{
		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UBlueprint* Existing = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).TryLoad()); IsValid(Existing))
		{
			return Existing;
		}

		UPackage* Package = CreatePackage(*AssetPath);
		if (!IsValid(Package)) return nullptr;

		const FString AssetName = FPackageName::GetShortName(AssetPath);
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);
		if (!IsValid(BP)) return nullptr;

		FAssetRegistryModule::AssetCreated(BP);
		BP->MarkPackageDirty();

		const FString PackageFileName = FPackageName::LongPackageNameToFilename(
			AssetPath, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::Save(Package, BP, *PackageFileName, SaveArgs);

		return BP;
	}

	// Add a user function graph with the given name. Returns the new graph.
	UEdGraph* DataEnvTest_AddFunctionGraph(UBlueprint* BP, const TCHAR* GraphName)
	{
		UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FName(GraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!IsValid(NewGraph)) return nullptr;
		// AddFunctionGraph<UClass> is a fully header-inline template
		// (BlueprintEditorUtils.h) whose only signature-dependent call resolves
		// to the exported virtual UEdGraphSchema_K2::CreateFunctionGraphTerminators
		// (UEdGraph&, UClass*); no exported explicit instantiation is involved,
		// and four production files in this module already compile this exact
		// instantiation (e.g. ClaireonBlueprintGraphTool_AddFunction.cpp).
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(
			BP, NewGraph, /*bIsUserCreated=*/true, /*SignatureFromObject=*/nullptr);
		return NewGraph;
	}

	UEdGraph* DataEnvTest_FindEventGraph(UBlueprint* BP)
	{
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (IsValid(G)) return G;
		}
		return nullptr;
	}

	// Spawn a UK2Node_DynamicCast into the event graph so there is a known node
	// with a stable GUID and a non-empty pin set to inspect.
	UK2Node_DynamicCast* DataEnvTest_AddDynamicCast(UBlueprint* BP)
	{
		UEdGraph* EventGraph = DataEnvTest_FindEventGraph(BP);
		if (!IsValid(EventGraph)) return nullptr;

		UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(EventGraph);
		Node->TargetType = AActor::StaticClass();
		Node->SetFlags(RF_Transactional);
		EventGraph->AddNode(Node, /*bUserAction=*/false, /*bSelectNewNode=*/false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		return Node;
	}

	// Open a Blueprint editing session on the asset via the Open tool.
	// Returns the session_id, or empty string on failure.
	FString DataEnvTest_OpenSession(const FString& AssetPath)
	{
		ClaireonBlueprintGraphTool_Open OpenTool;
		TSharedPtr<FJsonObject> OpenArgs = MakeShared<FJsonObject>();
		OpenArgs->SetStringField(TEXT("asset_path"), AssetPath);
		IClaireonTool::FToolResult OpenResult = OpenTool.Execute(OpenArgs);
		if (OpenResult.bIsError || !OpenResult.Data.IsValid()) return FString();
		FString SessionId;
		OpenResult.Data->TryGetStringField(TEXT("session_id"), SessionId);
		return SessionId;
	}

	// Find the entry with matching "name" in a graphs[] JSON array. Returns
	// true and fills the out fields on a hit. No UNTEST macros here -- callers
	// assert on the returned values.
	bool DataEnvTest_FindGraphEntry(
		const TArray<TSharedPtr<FJsonValue>>& GraphValues,
		const FString& GraphName,
		FString& OutType,
		int32& OutNodeCount)
	{
		for (const TSharedPtr<FJsonValue>& Value : GraphValues)
		{
			const TSharedPtr<FJsonObject>* EntryObj = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(EntryObj) || !EntryObj || !EntryObj->IsValid())
			{
				continue;
			}
			FString EntryName;
			if ((*EntryObj)->TryGetStringField(TEXT("name"), EntryName) && EntryName == GraphName)
			{
				(*EntryObj)->TryGetStringField(TEXT("type"), OutType);
				double NodeCountNumber = -1.0;
				(*EntryObj)->TryGetNumberField(TEXT("node_count"), NodeCountNumber);
				OutNodeCount = static_cast<int32>(NodeCountNumber);
				return true;
			}
		}
		return false;
	}
} // namespace ClaireonListGraphsDataEnvelopeTestsInternal

using namespace ClaireonListGraphsDataEnvelopeTestsInternal;

// ============================================================================
// Test 1: bp_list_graphs returns a structured Data object with a graphs[]
// array covering every graph, correct names/types/node counts, and a count
// field -- not a null Data (which the bridge would collapse to {}).
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, ListGraphsDataEnvelope, ListGraphs_DataHasGraphsArray, UNTEST_TIMEOUTMS(60000))
{
	DataEnvTest_CleanupAsset(DataEnvTest_ListGraphsBPPath);
	UBlueprint* BP = DataEnvTest_CreateActorBP(DataEnvTest_ListGraphsBPPath);
	UNTEST_ASSERT_PTR(BP);

	UEdGraph* FuncA = DataEnvTest_AddFunctionGraph(BP, TEXT("EnvTestFuncA"));
	UNTEST_ASSERT_PTR(FuncA);
	UEdGraph* FuncB = DataEnvTest_AddFunctionGraph(BP, TEXT("EnvTestFuncB"));
	UNTEST_ASSERT_PTR(FuncB);

	// Expected total straight from the Blueprint's graph lists (event graph +
	// UserConstructionScript + the two functions above, plus anything else the
	// factory added).
	int32 ExpectedGraphCount = 0;
	for (UEdGraph* G : BP->UbergraphPages)          { if (IsValid(G)) ++ExpectedGraphCount; }
	for (UEdGraph* G : BP->FunctionGraphs)          { if (IsValid(G)) ++ExpectedGraphCount; }
	for (UEdGraph* G : BP->MacroGraphs)             { if (IsValid(G)) ++ExpectedGraphCount; }
	for (UEdGraph* G : BP->DelegateSignatureGraphs) { if (IsValid(G)) ++ExpectedGraphCount; }
	UNTEST_ASSERT_TRUE(ExpectedGraphCount >= 3);

	ClaireonBlueprintGraphTool_ListGraphs Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), DataEnvTest_ListGraphsBPPath);
	IClaireonTool::FToolResult R = Tool.Execute(Args);

	UNTEST_ASSERT_FALSE(R.bIsError);
	// The core WI-4 assertion: Data must be a real object, not null.
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* GraphValues = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetArrayField(TEXT("graphs"), GraphValues));
	UNTEST_ASSERT_PTR(GraphValues);
	UNTEST_EXPECT_EQ(GraphValues->Num(), ExpectedGraphCount);

	double CountField = -1.0;
	UNTEST_EXPECT_TRUE(R.Data->TryGetNumberField(TEXT("count"), CountField));
	UNTEST_EXPECT_EQ(static_cast<int32>(CountField), ExpectedGraphCount);

	FString FoundType;
	int32 FoundNodeCount = -1;
	UNTEST_ASSERT_TRUE(DataEnvTest_FindGraphEntry(*GraphValues, TEXT("EnvTestFuncA"), FoundType, FoundNodeCount));
	UNTEST_EXPECT_EQ(FoundType, FString(TEXT("Function")));
	UNTEST_EXPECT_EQ(FoundNodeCount, FuncA->Nodes.Num());

	FoundType.Reset();
	FoundNodeCount = -1;
	UNTEST_ASSERT_TRUE(DataEnvTest_FindGraphEntry(*GraphValues, TEXT("EnvTestFuncB"), FoundType, FoundNodeCount));
	UNTEST_EXPECT_EQ(FoundType, FString(TEXT("Function")));
	UNTEST_EXPECT_EQ(FoundNodeCount, FuncB->Nodes.Num());

	// The human summary must be preserved alongside the structured payload.
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("EnvTestFuncA")));

	DataEnvTest_CleanupAsset(DataEnvTest_ListGraphsBPPath);
	co_return;
}

// ============================================================================
// Test 2: bp_inspect_node returns the full serialized node as structured Data
// (node_id + pins), with the pretty-printed JSON summary kept as before.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, ListGraphsDataEnvelope, InspectNode_DataHasNodeIdAndPins, UNTEST_TIMEOUTMS(60000))
{
	DataEnvTest_CleanupAsset(DataEnvTest_InspectNodeBPPath);
	UBlueprint* BP = DataEnvTest_CreateActorBP(DataEnvTest_InspectNodeBPPath);
	UNTEST_ASSERT_PTR(BP);

	UK2Node_DynamicCast* Node = DataEnvTest_AddDynamicCast(BP);
	UNTEST_ASSERT_PTR(Node);
	UNTEST_ASSERT_TRUE(Node->Pins.Num() > 0);

	const FString NodeGuidStr = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

	const FString SessionId = DataEnvTest_OpenSession(DataEnvTest_InspectNodeBPPath);
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	ClaireonBlueprintGraphTool_InspectNode Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Args->SetStringField(TEXT("node_guid"), NodeGuidStr);
	IClaireonTool::FToolResult R = Tool.Execute(Args);

	const bool bIsError = R.bIsError;
	const bool bDataValid = R.Data.IsValid();

	// Tear down the session before asserting so a failed expectation cannot
	// leak a live session into subsequent tests.
	FClaireonSessionManager::Get().CloseSession(SessionId);

	UNTEST_ASSERT_FALSE(bIsError);
	// The core WI-4 assertion: Data must be a real object, not null.
	UNTEST_ASSERT_TRUE(bDataValid);

	FString DataNodeId;
	UNTEST_EXPECT_TRUE(R.Data->TryGetStringField(TEXT("node_id"), DataNodeId));
	UNTEST_EXPECT_EQ(DataNodeId, NodeGuidStr);

	const TArray<TSharedPtr<FJsonValue>>* PinValues = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetArrayField(TEXT("pins"), PinValues));
	UNTEST_ASSERT_PTR(PinValues);
	UNTEST_EXPECT_EQ(PinValues->Num(), Node->Pins.Num());

	// Summary contract unchanged: still the pretty-printed node JSON.
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("node_id")));
	UNTEST_EXPECT_TRUE(R.Summary.Contains(NodeGuidStr));

	DataEnvTest_CleanupAsset(DataEnvTest_InspectNodeBPPath);
	co_return;
}

#endif // WITH_UNTESTED
