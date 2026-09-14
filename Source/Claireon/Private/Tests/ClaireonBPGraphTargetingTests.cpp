// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Headless tests for named-graph targeting and default graph-detail member references and links.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/ClaireonTool_GetBlueprintGraph.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonSessionManager.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_IfThenElse.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonBPGraphTargetingTests_anon
{
	static const TCHAR* BGT_Path_AddNode_SecondGraph   = TEXT("/Game/__MCPTests/BP_BPGraphTargeting_SecondGraph");
	static const TCHAR* BGT_Path_AddNode_BadTarget     = TEXT("/Game/__MCPTests/BP_BPGraphTargeting_BadTarget");
	static const TCHAR* BGT_Path_AddNode_NoTarget      = TEXT("/Game/__MCPTests/BP_BPGraphTargeting_NoTarget");
	static const TCHAR* BGT_Path_GetGraph_DefaultLevel = TEXT("/Game/__MCPTests/BP_BPGraphTargeting_DefaultLevel");

	static const TCHAR* BGT_SecondGraphName = TEXT("BGT_SecondGraph");
	static const TCHAR* BGT_FuncGraphName   = TEXT("BGT_GetGraphFunc");

	// Release any session auto-opened on the asset, then force-delete it.
	static void BGT_CleanupAsset(const FString& AssetPath)
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

	static UBlueprint* BGT_CreateActorBP(const FString& AssetPath)
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

	static UEdGraph* BGT_FindEventGraph(UBlueprint* BP)
	{
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (IsValid(G)) return G;
		}
		return nullptr;
	}

	// Add a graph distinct from the default to make retargeting observable.
	static UEdGraph* BGT_AddNamedFunctionGraph(UBlueprint* BP, const TCHAR* GraphName)
	{
		UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FName(GraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!IsValid(FuncGraph))
		{
			return nullptr;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, FuncGraph, /*bIsUserCreated=*/true, /*SignatureFromObject=*/nullptr);
		return FuncGraph;
	}

	// Number of nodes of exactly-or-derived class T in ONE graph.
	template <typename T>
	static int32 BGT_CountNodesOfClassInGraph(const UEdGraph* Graph)
	{
		if (!IsValid(Graph)) return 0;
		int32 Count = 0;
		for (UEdGraphNode* Node : Graph->Nodes)
		{
			if (Cast<T>(Node)) ++Count;
		}
		return Count;
	}

	// Count across all graphs to catch unintended fallback creation.
	template <typename T>
	static int32 BGT_CountNodesOfClassAnywhere(UBlueprint* BP)
	{
		TArray<UEdGraph*> AllGraphs;
		BP->GetAllGraphs(AllGraphs);
		int32 Count = 0;
		for (const UEdGraph* Graph : AllGraphs)
		{
			Count += BGT_CountNodesOfClassInGraph<T>(Graph);
		}
		return Count;
	}


	static TSharedPtr<FJsonObject> BGT_FirstGraphObj(const IClaireonTool::FToolResult& ToolResult)
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

	static TSharedPtr<FJsonObject> BGT_FindNodeByClass(const TSharedPtr<FJsonObject>& GraphObj, const FString& NodeClass)
	{
		if (!GraphObj.IsValid()) return nullptr;
		const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
		if (!GraphObj->TryGetArrayField(TEXT("nodes"), NodeValues)) return nullptr;
		for (const TSharedPtr<FJsonValue>& NodeValue : *NodeValues)
		{
			TSharedPtr<FJsonObject> NodeObj = NodeValue->AsObject();
			FString FoundClass;
			if (NodeObj.IsValid()
				&& NodeObj->TryGetStringField(TEXT("node_class"), FoundClass)
				&& FoundClass == NodeClass)
			{
				return NodeObj;
			}
		}
		return nullptr;
	}

	static TSharedPtr<FJsonObject> BGT_FindPin(const TSharedPtr<FJsonObject>& NodeObj, const FString& PinName)
	{
		if (!NodeObj.IsValid()) return nullptr;
		const TArray<TSharedPtr<FJsonValue>>* PinValues = nullptr;
		if (!NodeObj->TryGetArrayField(TEXT("pins"), PinValues)) return nullptr;
		for (const TSharedPtr<FJsonValue>& PinValue : *PinValues)
		{
			TSharedPtr<FJsonObject> PinObj = PinValue->AsObject();
			if (PinObj.IsValid() && PinObj->GetStringField(TEXT("pin_name")) == PinName)
			{
				return PinObj;
			}
		}
		return nullptr;
	}
}

// Adding to a named graph updates both node placement and session target.
UNTEST_UNIT_OPTS(Claireon, BPGraphTargeting, AddNode_TargetGraphSwitchesSessionAndPlacesNode, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonBPGraphTargetingTests_anon;

	BGT_CleanupAsset(BGT_Path_AddNode_SecondGraph);
	UBlueprint* BP = BGT_CreateActorBP(BGT_Path_AddNode_SecondGraph);
	UNTEST_ASSERT_PTR(BP);

	UEdGraph* EventGraph = BGT_FindEventGraph(BP);
	UNTEST_ASSERT_PTR(EventGraph);
	UEdGraph* SecondGraph = BGT_AddNamedFunctionGraph(BP, BGT_SecondGraphName);
	UNTEST_ASSERT_PTR(SecondGraph);

	ClaireonBlueprintGraphTool_AddNode Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), BGT_Path_AddNode_SecondGraph);
	Args->SetStringField(TEXT("node_type"), TEXT("Branch"));
	Args->SetStringField(TEXT("target_graph"), BGT_SecondGraphName);

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	UNTEST_EXPECT_EQ(BGT_CountNodesOfClassInGraph<UK2Node_IfThenElse>(SecondGraph), 1);
	UNTEST_EXPECT_EQ(BGT_CountNodesOfClassInGraph<UK2Node_IfThenElse>(EventGraph), 0);

	FString ReportedGraphName;
	UNTEST_ASSERT_TRUE(R.Data->TryGetStringField(TEXT("graph_name"), ReportedGraphName));
	UNTEST_EXPECT_TRUE(ReportedGraphName == BGT_SecondGraphName);

	FString ReportedTargetGraph;
	UNTEST_ASSERT_TRUE(R.Data->TryGetStringField(TEXT("target_graph"), ReportedTargetGraph));
	UNTEST_EXPECT_TRUE(ReportedTargetGraph == BGT_SecondGraphName);

	bool bSwitched = false;
	UNTEST_ASSERT_TRUE(R.Data->TryGetBoolField(TEXT("switched_to_target_graph"), bSwitched));
	UNTEST_EXPECT_TRUE(bSwitched);

	BGT_CleanupAsset(BGT_Path_AddNode_SecondGraph);
	co_return;
}

// An unknown graph must report available graphs and create no node anywhere.
UNTEST_UNIT_OPTS(Claireon, BPGraphTargeting, AddNode_TargetGraphMissingErrorsAndCreatesNothing, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonBPGraphTargetingTests_anon;

	BGT_CleanupAsset(BGT_Path_AddNode_BadTarget);
	UBlueprint* BP = BGT_CreateActorBP(BGT_Path_AddNode_BadTarget);
	UNTEST_ASSERT_PTR(BP);

	ClaireonBlueprintGraphTool_AddNode Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), BGT_Path_AddNode_BadTarget);
	Args->SetStringField(TEXT("node_type"), TEXT("Branch"));
	Args->SetStringField(TEXT("target_graph"), TEXT("Bogus_DoesNotExist_BGT"));

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("Bogus_DoesNotExist_BGT")));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("not found in Blueprint")));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("EventGraph")));

	UNTEST_EXPECT_EQ(BGT_CountNodesOfClassAnywhere<UK2Node_IfThenElse>(BP), 0);

	BGT_CleanupAsset(BGT_Path_AddNode_BadTarget);
	co_return;
}

// Omitting target_graph uses the current graph without reporting a switch.
UNTEST_UNIT_OPTS(Claireon, BPGraphTargeting, AddNode_NoTargetGraphUnchangedBehavior, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonBPGraphTargetingTests_anon;

	BGT_CleanupAsset(BGT_Path_AddNode_NoTarget);
	UBlueprint* BP = BGT_CreateActorBP(BGT_Path_AddNode_NoTarget);
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* EventGraph = BGT_FindEventGraph(BP);
	UNTEST_ASSERT_PTR(EventGraph);

	ClaireonBlueprintGraphTool_AddNode Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), BGT_Path_AddNode_NoTarget);
	Args->SetStringField(TEXT("node_type"), TEXT("Branch"));
	// target_graph deliberately omitted.

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	UNTEST_EXPECT_EQ(BGT_CountNodesOfClassInGraph<UK2Node_IfThenElse>(EventGraph), 1);

	UNTEST_EXPECT_FALSE(R.Data->HasField(TEXT("target_graph")));
	UNTEST_EXPECT_FALSE(R.Data->HasField(TEXT("switched_to_target_graph")));

	BGT_CleanupAsset(BGT_Path_AddNode_NoTarget);
	co_return;
}

// Default exec detail must include both member references and linked pins.
UNTEST_UNIT_OPTS(Claireon, BPGraphTargeting, GetGraph_DefaultDetailLevelCarriesMemberReferenceAndLinkedTo, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonBPGraphTargetingTests_anon;

	BGT_CleanupAsset(BGT_Path_GetGraph_DefaultLevel);
	UBlueprint* BP = BGT_CreateActorBP(BGT_Path_GetGraph_DefaultLevel);
	UNTEST_ASSERT_PTR(BP);

	UEdGraph* FuncGraph = BGT_AddNamedFunctionGraph(BP, BGT_FuncGraphName);
	UNTEST_ASSERT_PTR(FuncGraph);

	UK2Node_FunctionEntry* Entry = nullptr;
	for (UEdGraphNode* GraphNode : FuncGraph->Nodes)
	{
		Entry = Cast<UK2Node_FunctionEntry>(GraphNode);
		if (IsValid(Entry)) break;
	}
	UNTEST_ASSERT_PTR(Entry);

	// Wire the call from the entry so its payload has a real link to inspect.
	UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(FuncGraph);
	Call->FunctionReference.SetExternalMember(
		GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), UKismetSystemLibrary::StaticClass());
	FuncGraph->AddNode(Call, /*bUserAction=*/false, /*bSelectNewNode=*/false);
	Call->CreateNewGuid();
	Call->PostPlacedNewNode();
	Call->AllocateDefaultPins();

	UEdGraphPin* EntryThen = Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
	UEdGraphPin* CallExec = Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
	UNTEST_ASSERT_PTR(EntryThen);
	UNTEST_ASSERT_PTR(CallExec);
	EntryThen->MakeLinkTo(CallExec);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);

	ClaireonTool_GetBlueprintGraph Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), BGT_Path_GetGraph_DefaultLevel);
	Args->SetStringField(TEXT("graph_name"), BGT_FuncGraphName);
	// node_detail_level deliberately omitted -- exercising the DEFAULT ('exec').

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);

	TSharedPtr<FJsonObject> GraphObj = BGT_FirstGraphObj(R);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());

	TSharedPtr<FJsonObject> CallNodeObj = BGT_FindNodeByClass(GraphObj, TEXT("K2Node_CallFunction"));
	UNTEST_ASSERT_TRUE(CallNodeObj.IsValid());

	const TSharedPtr<FJsonObject>* FunctionRefObj = nullptr;
	UNTEST_ASSERT_TRUE(CallNodeObj->TryGetObjectField(TEXT("function_reference"), FunctionRefObj));
	UNTEST_EXPECT_TRUE((*FunctionRefObj)->GetStringField(TEXT("member_name")) == TEXT("PrintString"));

	TSharedPtr<FJsonObject> ExecPinObj = BGT_FindPin(CallNodeObj, TEXT("execute"));
	UNTEST_ASSERT_TRUE(ExecPinObj.IsValid());
	const TArray<TSharedPtr<FJsonValue>>* LinkedToValues = nullptr;
	UNTEST_ASSERT_TRUE(ExecPinObj->TryGetArrayField(TEXT("linked_to"), LinkedToValues));
	UNTEST_EXPECT_TRUE(LinkedToValues->Num() == 1);

	BGT_CleanupAsset(BGT_Path_GetGraph_DefaultLevel);
	co_return;
}

#endif // WITH_UNTESTED
