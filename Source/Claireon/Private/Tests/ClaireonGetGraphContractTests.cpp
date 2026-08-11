// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Contract tests for claireon.bp_get_graph (WI-3).
//
// Builds a throwaway Blueprint under /Game/__MCPTests/ with a small wired
// function graph (FunctionEntry -> CallFunction(PrintString) <- VariableGet),
// drives the tool through its JSON Execute entry, and asserts the payload
// contracts:
//   - format='t3d' emits a T3D block ("Begin Object").
//   - node_detail_level='full' carries function_reference / variable_reference.
//   - every connections[] endpoint GUID string-matches some nodes[].node_id
//     (single DigitsWithHyphens format everywhere).
//   - max_nodes truncation keeps edges to out-of-set nodes, flagged
//     target_in_set:false / source_in_set:false, instead of dropping them.
//   - include_pin_defaults=false suppresses default_value emission.
//   - connected pins carry linked_to [{node_id, pin_name}] at full/exec detail.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonTool_GetBlueprintGraph.h"
#include "Tools/IClaireonTool.h"

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
#include "K2Node_VariableGet.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonGetGraphContractTestsInternal
{
	// All anon-namespace symbols carry the GetGraphContract discriminator prefix
	// (anon namespaces are not isolation under unity batching).

	static const TCHAR* kGetGraphContractFuncName = TEXT("GetGraphContractFunc");
	static const TCHAR* kGetGraphContractVarName  = TEXT("ContractTestString");

	static const TCHAR* kGetGraphContractBPPath_T3D          = TEXT("/Game/__MCPTests/BP_GetGraphContract_T3D");
	static const TCHAR* kGetGraphContractBPPath_MemberRefs   = TEXT("/Game/__MCPTests/BP_GetGraphContract_MemberRefs");
	static const TCHAR* kGetGraphContractBPPath_GuidFormat   = TEXT("/Game/__MCPTests/BP_GetGraphContract_GuidFormat");
	static const TCHAR* kGetGraphContractBPPath_MaxNodes     = TEXT("/Game/__MCPTests/BP_GetGraphContract_MaxNodes");
	static const TCHAR* kGetGraphContractBPPath_PinDefaults  = TEXT("/Game/__MCPTests/BP_GetGraphContract_PinDefaults");

	struct FGetGraphContractFixture
	{
		UBlueprint* BP = nullptr;
		UEdGraph* FuncGraph = nullptr;
		UK2Node_FunctionEntry* Entry = nullptr;
		UK2Node_CallFunction* Call = nullptr;
		UK2Node_VariableGet* VarGet = nullptr;
	};

	void GetGraphContractCleanupAsset(const FString& AssetPath)
	{
		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> AssetsToDelete;
			AssetsToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
		}
	}

	// Builds the fixture Blueprint. Returns false with OutError set on any
	// failure so UNTEST asserts stay in the test body (they cannot appear in
	// non-coroutine helpers).
	bool GetGraphContractBuildFixture(const FString& AssetPath, FGetGraphContractFixture& Out, FString& OutError)
	{
		GetGraphContractCleanupAsset(AssetPath);

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

		// Member string variable BEFORE the VariableGet node so the skeleton
		// class carries the property when the node allocates its pins.
		FEdGraphPinType StringPinType;
		StringPinType.PinCategory = UEdGraphSchema_K2::PC_String;
		if (!FBlueprintEditorUtils::AddMemberVariable(BP, FName(kGetGraphContractVarName), StringPinType))
		{
			OutError = TEXT("AddMemberVariable failed");
			return false;
		}

		// Function graph with its auto-created FunctionEntry node.
		UEdGraph* FuncGraph = FBlueprintEditorUtils::CreateNewGraph(
			BP, FName(kGetGraphContractFuncName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
		if (!IsValid(FuncGraph))
		{
			OutError = TEXT("CreateNewGraph failed");
			return false;
		}
		FBlueprintEditorUtils::AddFunctionGraph<UClass>(BP, FuncGraph, /*bIsUserCreated=*/true, /*SignatureFromObject=*/nullptr);

		UK2Node_FunctionEntry* Entry = nullptr;
		for (UEdGraphNode* GraphNode : FuncGraph->Nodes)
		{
			Entry = Cast<UK2Node_FunctionEntry>(GraphNode);
			if (IsValid(Entry))
			{
				break;
			}
		}
		if (!IsValid(Entry))
		{
			OutError = TEXT("Function graph has no K2Node_FunctionEntry");
			return false;
		}

		// CallFunction node: KismetSystemLibrary::PrintString (exec pins + string input
		// + several unconnected pins with non-empty defaults).
		UK2Node_CallFunction* Call = NewObject<UK2Node_CallFunction>(FuncGraph);
		Call->FunctionReference.SetExternalMember(
			GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary, PrintString), UKismetSystemLibrary::StaticClass());
		FuncGraph->AddNode(Call, /*bUserAction=*/false, /*bSelectNewNode=*/false);
		Call->CreateNewGuid();
		Call->PostPlacedNewNode();
		Call->AllocateDefaultPins();
		Call->NodePosX = 300;

		// VariableGet on the member string variable.
		UK2Node_VariableGet* VarGet = NewObject<UK2Node_VariableGet>(FuncGraph);
		VarGet->VariableReference.SetSelfMember(FName(kGetGraphContractVarName));
		FuncGraph->AddNode(VarGet, /*bUserAction=*/false, /*bSelectNewNode=*/false);
		VarGet->CreateNewGuid();
		VarGet->PostPlacedNewNode();
		VarGet->AllocateDefaultPins();
		VarGet->NodePosX = 100;
		VarGet->NodePosY = 200;

		// Wire: Entry.then -> Call.execute; VarGet.<var> -> Call.InString.
		UEdGraphPin* EntryThen = Entry->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* CallExec = Call->FindPin(UEdGraphSchema_K2::PN_Execute, EGPD_Input);
		UEdGraphPin* VarOut = VarGet->FindPin(FName(kGetGraphContractVarName), EGPD_Output);
		UEdGraphPin* CallInString = Call->FindPin(FName(TEXT("InString")), EGPD_Input);
		if (!EntryThen || !CallExec || !VarOut || !CallInString)
		{
			OutError = FString::Printf(TEXT("Fixture pin lookup failed (then=%d execute=%d varout=%d InString=%d)"),
				EntryThen != nullptr, CallExec != nullptr, VarOut != nullptr, CallInString != nullptr);
			return false;
		}
		EntryThen->MakeLinkTo(CallExec);
		VarOut->MakeLinkTo(CallInString);

		BP->MarkPackageDirty();

		// Save to disk so the tool's load path matches production usage.
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
		Out.FuncGraph = FuncGraph;
		Out.Entry = Entry;
		Out.Call = Call;
		Out.VarGet = VarGet;
		return true;
	}

	TSharedPtr<FJsonObject> GetGraphContractMakeArgs(const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("graph_name"), kGetGraphContractFuncName);
		return Args;
	}

	// Returns the first graph object from a successful result, or null.
	TSharedPtr<FJsonObject> GetGraphContractFirstGraph(const IClaireonTool::FToolResult& ToolResult)
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

	// Finds the first node object whose node_class matches, or null.
	TSharedPtr<FJsonObject> GetGraphContractFindNodeByClass(const TSharedPtr<FJsonObject>& GraphObj, const FString& NodeClass)
	{
		if (!GraphObj.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
		if (!GraphObj->TryGetArrayField(TEXT("nodes"), NodeValues))
		{
			return nullptr;
		}
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

	// Finds the pin object with the given name on a node object, or null.
	TSharedPtr<FJsonObject> GetGraphContractFindPin(const TSharedPtr<FJsonObject>& NodeObj, const FString& PinName)
	{
		if (!NodeObj.IsValid())
		{
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* PinValues = nullptr;
		if (!NodeObj->TryGetArrayField(TEXT("pins"), PinValues))
		{
			return nullptr;
		}
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

	// Collects every nodes[].node_id string into OutIds. Returns false on shape errors.
	bool GetGraphContractCollectNodeIds(const TSharedPtr<FJsonObject>& GraphObj, TSet<FString>& OutIds)
	{
		if (!GraphObj.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
		if (!GraphObj->TryGetArrayField(TEXT("nodes"), NodeValues))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& NodeValue : *NodeValues)
		{
			TSharedPtr<FJsonObject> NodeObj = NodeValue->AsObject();
			if (!NodeObj.IsValid())
			{
				return false;
			}
			FString NodeId;
			if (!NodeObj->TryGetStringField(TEXT("node_id"), NodeId) || NodeId.IsEmpty())
			{
				return false;
			}
			OutIds.Add(NodeId);
		}
		return true;
	}

	// True if every connections[] endpoint GUID string-matches a member of NodeIds.
	// OutDetail carries the first offending edge for the assert message.
	bool GetGraphContractAllConnectionEndpointsInSet(const TSharedPtr<FJsonObject>& GraphObj, const TSet<FString>& NodeIds, FString& OutDetail)
	{
		if (!GraphObj.IsValid())
		{
			OutDetail = TEXT("graph object missing");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* ConnValues = nullptr;
		if (!GraphObj->TryGetArrayField(TEXT("connections"), ConnValues))
		{
			OutDetail = TEXT("connections array missing");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& ConnValue : *ConnValues)
		{
			TSharedPtr<FJsonObject> ConnObj = ConnValue->AsObject();
			if (!ConnObj.IsValid())
			{
				OutDetail = TEXT("connection entry is not an object");
				return false;
			}
			const FString FromNode = ConnObj->GetStringField(TEXT("from_node"));
			const FString ToNode = ConnObj->GetStringField(TEXT("to_node"));
			if (!NodeIds.Contains(FromNode))
			{
				OutDetail = FString::Printf(TEXT("from_node %s not in nodes[].node_id set"), *FromNode);
				return false;
			}
			if (!NodeIds.Contains(ToNode))
			{
				OutDetail = FString::Printf(TEXT("to_node %s not in nodes[].node_id set"), *ToNode);
				return false;
			}
		}
		return true;
	}

	// True if any pin object anywhere in the graph payload carries default_value.
	bool GetGraphContractAnyPinHasDefaultValue(const TSharedPtr<FJsonObject>& GraphObj)
	{
		if (!GraphObj.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* NodeValues = nullptr;
		if (!GraphObj->TryGetArrayField(TEXT("nodes"), NodeValues))
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& NodeValue : *NodeValues)
		{
			TSharedPtr<FJsonObject> NodeObj = NodeValue->AsObject();
			if (!NodeObj.IsValid())
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* PinValues = nullptr;
			if (!NodeObj->TryGetArrayField(TEXT("pins"), PinValues))
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& PinValue : *PinValues)
			{
				TSharedPtr<FJsonObject> PinObj = PinValue->AsObject();
				if (PinObj.IsValid() && PinObj->HasField(TEXT("default_value")))
				{
					return true;
				}
			}
		}
		return false;
	}

	// Validates the max_nodes=1 truncation contract:
	// - exactly one node in nodes[]
	// - at least one connection referencing an out-of-set node
	// - every connection touches the surviving node, and its out-of-set side is
	//   flagged target_in_set:false (outgoing) or source_in_set:false (incoming).
	bool GetGraphContractValidateTruncatedEdges(const TSharedPtr<FJsonObject>& GraphObj, FString& OutDetail)
	{
		TSet<FString> NodeIds;
		if (!GetGraphContractCollectNodeIds(GraphObj, NodeIds))
		{
			OutDetail = TEXT("failed to collect node ids");
			return false;
		}
		if (NodeIds.Num() != 1)
		{
			OutDetail = FString::Printf(TEXT("expected exactly 1 node, got %d"), NodeIds.Num());
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* ConnValues = nullptr;
		if (!GraphObj->TryGetArrayField(TEXT("connections"), ConnValues))
		{
			OutDetail = TEXT("connections array missing");
			return false;
		}

		int32 OutOfSetEdgeCount = 0;
		for (const TSharedPtr<FJsonValue>& ConnValue : *ConnValues)
		{
			TSharedPtr<FJsonObject> ConnObj = ConnValue->AsObject();
			if (!ConnObj.IsValid())
			{
				OutDetail = TEXT("connection entry is not an object");
				return false;
			}
			const FString FromNode = ConnObj->GetStringField(TEXT("from_node"));
			const FString ToNode = ConnObj->GetStringField(TEXT("to_node"));
			const bool bFromInSet = NodeIds.Contains(FromNode);
			const bool bToInSet = NodeIds.Contains(ToNode);
			if (!bFromInSet && !bToInSet)
			{
				OutDetail = TEXT("connection touches no in-set node");
				return false;
			}
			if (!bToInSet)
			{
				bool bTargetInSet = true;
				if (!ConnObj->TryGetBoolField(TEXT("target_in_set"), bTargetInSet) || bTargetInSet)
				{
					OutDetail = FString::Printf(TEXT("edge to out-of-set node %s lacks target_in_set:false"), *ToNode);
					return false;
				}
				++OutOfSetEdgeCount;
			}
			if (!bFromInSet)
			{
				bool bSourceInSet = true;
				if (!ConnObj->TryGetBoolField(TEXT("source_in_set"), bSourceInSet) || bSourceInSet)
				{
					OutDetail = FString::Printf(TEXT("edge from out-of-set node %s lacks source_in_set:false"), *FromNode);
					return false;
				}
				++OutOfSetEdgeCount;
			}
		}

		if (OutOfSetEdgeCount == 0)
		{
			OutDetail = TEXT("no out-of-set edges were emitted; truncated edges are being dropped");
			return false;
		}
		return true;
	}
} // namespace ClaireonGetGraphContractTestsInternal

using namespace ClaireonGetGraphContractTestsInternal;

// ============================================================================
// Test 1: format='t3d' -> the graph payload carries a T3D block.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GetGraphContract, Format_T3DEmitsBeginObject, UNTEST_TIMEOUTMS(60000))
{
	FGetGraphContractFixture Fixture;
	FString FixtureError;
	const bool bBuilt = GetGraphContractBuildFixture(kGetGraphContractBPPath_T3D, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[GetGraphContract] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);

	ClaireonTool_GetBlueprintGraph Tool;
	TSharedPtr<FJsonObject> Args = GetGraphContractMakeArgs(kGetGraphContractBPPath_T3D);
	Args->SetStringField(TEXT("format"), TEXT("t3d"));
	IClaireonTool::FToolResult ToolResult = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(ToolResult.bIsError);

	TSharedPtr<FJsonObject> GraphObj = GetGraphContractFirstGraph(ToolResult);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());

	FString T3DText;
	UNTEST_ASSERT_TRUE(GraphObj->TryGetStringField(TEXT("t3d"), T3DText));
	UNTEST_EXPECT_TRUE(T3DText.Contains(TEXT("Begin Object")));

	GetGraphContractCleanupAsset(kGetGraphContractBPPath_T3D);
	co_return;
}

// ============================================================================
// Test 2: node_detail_level='full' -> CallFunction node carries its
// function_reference and VariableGet carries its variable_reference.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GetGraphContract, FullDetail_EmitsMemberReferences, UNTEST_TIMEOUTMS(60000))
{
	FGetGraphContractFixture Fixture;
	FString FixtureError;
	const bool bBuilt = GetGraphContractBuildFixture(kGetGraphContractBPPath_MemberRefs, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[GetGraphContract] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);

	ClaireonTool_GetBlueprintGraph Tool;
	TSharedPtr<FJsonObject> Args = GetGraphContractMakeArgs(kGetGraphContractBPPath_MemberRefs);
	Args->SetStringField(TEXT("node_detail_level"), TEXT("full"));
	IClaireonTool::FToolResult ToolResult = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(ToolResult.bIsError);

	TSharedPtr<FJsonObject> GraphObj = GetGraphContractFirstGraph(ToolResult);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());

	TSharedPtr<FJsonObject> CallNodeObj = GetGraphContractFindNodeByClass(GraphObj, TEXT("K2Node_CallFunction"));
	UNTEST_ASSERT_TRUE(CallNodeObj.IsValid());
	const TSharedPtr<FJsonObject>* FunctionRefObj = nullptr;
	UNTEST_ASSERT_TRUE(CallNodeObj->TryGetObjectField(TEXT("function_reference"), FunctionRefObj));
	UNTEST_EXPECT_TRUE((*FunctionRefObj)->GetStringField(TEXT("member_name")) == TEXT("PrintString"));

	TSharedPtr<FJsonObject> VarNodeObj = GetGraphContractFindNodeByClass(GraphObj, TEXT("K2Node_VariableGet"));
	UNTEST_ASSERT_TRUE(VarNodeObj.IsValid());
	const TSharedPtr<FJsonObject>* VariableRefObj = nullptr;
	UNTEST_ASSERT_TRUE(VarNodeObj->TryGetObjectField(TEXT("variable_reference"), VariableRefObj));
	UNTEST_EXPECT_TRUE((*VariableRefObj)->GetStringField(TEXT("member_name")) == kGetGraphContractVarName);

	GetGraphContractCleanupAsset(kGetGraphContractBPPath_MemberRefs);
	co_return;
}

// ============================================================================
// Test 3: every connections[] endpoint GUID string-matches some nodes[].node_id
// (single DigitsWithHyphens GUID format), and connected pins carry linked_to
// entries whose node_id also string-matches (exec detail here, full via Test 2's
// level shares the same emission path).
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GetGraphContract, Connections_GuidFormatMatchesNodeIds, UNTEST_TIMEOUTMS(60000))
{
	FGetGraphContractFixture Fixture;
	FString FixtureError;
	const bool bBuilt = GetGraphContractBuildFixture(kGetGraphContractBPPath_GuidFormat, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[GetGraphContract] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);

	ClaireonTool_GetBlueprintGraph Tool;
	// Default node_detail_level ('exec'), no cap: all three nodes returned.
	IClaireonTool::FToolResult ToolResult = Tool.Execute(GetGraphContractMakeArgs(kGetGraphContractBPPath_GuidFormat));
	UNTEST_ASSERT_FALSE(ToolResult.bIsError);

	TSharedPtr<FJsonObject> GraphObj = GetGraphContractFirstGraph(ToolResult);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());

	TSet<FString> NodeIds;
	UNTEST_ASSERT_TRUE(GetGraphContractCollectNodeIds(GraphObj, NodeIds));
	UNTEST_ASSERT_TRUE(NodeIds.Num() >= 3);

	// Both authored edges (exec + data) must be present.
	const TArray<TSharedPtr<FJsonValue>>* ConnValues = nullptr;
	UNTEST_ASSERT_TRUE(GraphObj->TryGetArrayField(TEXT("connections"), ConnValues));
	UNTEST_ASSERT_TRUE(ConnValues->Num() >= 2);

	FString MismatchDetail;
	const bool bAllEndpointsMatch = GetGraphContractAllConnectionEndpointsInSet(GraphObj, NodeIds, MismatchDetail);
	if (!bAllEndpointsMatch)
	{
		UE_LOG(LogTemp, Error, TEXT("[GetGraphContract] connection endpoint mismatch: %s"), *MismatchDetail);
	}
	UNTEST_ASSERT_TRUE(bAllEndpointsMatch);

	// linked_to contract at exec detail: the entry node's 'then' pin lists the
	// CallFunction node's GUID (in the same format as node_id) and far pin name.
	TSharedPtr<FJsonObject> EntryNodeObj = GetGraphContractFindNodeByClass(GraphObj, TEXT("K2Node_FunctionEntry"));
	UNTEST_ASSERT_TRUE(EntryNodeObj.IsValid());
	TSharedPtr<FJsonObject> ThenPinObj = GetGraphContractFindPin(EntryNodeObj, TEXT("then"));
	UNTEST_ASSERT_TRUE(ThenPinObj.IsValid());
	const TArray<TSharedPtr<FJsonValue>>* LinkedToValues = nullptr;
	UNTEST_ASSERT_TRUE(ThenPinObj->TryGetArrayField(TEXT("linked_to"), LinkedToValues));
	UNTEST_ASSERT_TRUE(LinkedToValues->Num() == 1);
	TSharedPtr<FJsonObject> LinkObj = (*LinkedToValues)[0]->AsObject();
	UNTEST_ASSERT_TRUE(LinkObj.IsValid());
	const FString ExpectedCallGuid = Fixture.Call->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);
	UNTEST_EXPECT_TRUE(LinkObj->GetStringField(TEXT("node_id")) == ExpectedCallGuid);
	UNTEST_EXPECT_TRUE(LinkObj->GetStringField(TEXT("pin_name")) == TEXT("execute"));
	UNTEST_EXPECT_TRUE(NodeIds.Contains(LinkObj->GetStringField(TEXT("node_id"))));

	GetGraphContractCleanupAsset(kGetGraphContractBPPath_GuidFormat);
	co_return;
}

// ============================================================================
// Test 4: max_nodes=1 -> the surviving node's edges are still present and are
// marked out-of-set (target_in_set:false / source_in_set:false), not dropped.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GetGraphContract, MaxNodesOne_KeepsOutOfSetEdges, UNTEST_TIMEOUTMS(60000))
{
	FGetGraphContractFixture Fixture;
	FString FixtureError;
	const bool bBuilt = GetGraphContractBuildFixture(kGetGraphContractBPPath_MaxNodes, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[GetGraphContract] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);

	ClaireonTool_GetBlueprintGraph Tool;
	TSharedPtr<FJsonObject> Args = GetGraphContractMakeArgs(kGetGraphContractBPPath_MaxNodes);
	Args->SetNumberField(TEXT("max_nodes"), 1);
	IClaireonTool::FToolResult ToolResult = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(ToolResult.bIsError);

	TSharedPtr<FJsonObject> GraphObj = GetGraphContractFirstGraph(ToolResult);
	UNTEST_ASSERT_TRUE(GraphObj.IsValid());

	FString TruncationDetail;
	const bool bTruncatedEdgesValid = GetGraphContractValidateTruncatedEdges(GraphObj, TruncationDetail);
	if (!bTruncatedEdgesValid)
	{
		UE_LOG(LogTemp, Error, TEXT("[GetGraphContract] truncation contract violated: %s"), *TruncationDetail);
	}
	UNTEST_ASSERT_TRUE(bTruncatedEdgesValid);

	GetGraphContractCleanupAsset(kGetGraphContractBPPath_MaxNodes);
	co_return;
}

// ============================================================================
// Test 5: include_pin_defaults=false suppresses default_value everywhere;
// the default (omitted) keeps emitting defaults at 'full' detail.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GetGraphContract, IncludePinDefaultsFalse_SuppressesDefaults, UNTEST_TIMEOUTMS(60000))
{
	FGetGraphContractFixture Fixture;
	FString FixtureError;
	const bool bBuilt = GetGraphContractBuildFixture(kGetGraphContractBPPath_PinDefaults, Fixture, FixtureError);
	if (!bBuilt)
	{
		UE_LOG(LogTemp, Error, TEXT("[GetGraphContract] fixture build failed: %s"), *FixtureError);
	}
	UNTEST_ASSERT_TRUE(bBuilt);

	ClaireonTool_GetBlueprintGraph Tool;

	// Baseline: full detail with the parameter omitted -> PrintString's
	// unconnected pins (bPrintToScreen/bPrintToLog/Duration/...) surface defaults.
	TSharedPtr<FJsonObject> BaselineArgs = GetGraphContractMakeArgs(kGetGraphContractBPPath_PinDefaults);
	BaselineArgs->SetStringField(TEXT("node_detail_level"), TEXT("full"));
	IClaireonTool::FToolResult BaselineResult = Tool.Execute(BaselineArgs);
	UNTEST_ASSERT_FALSE(BaselineResult.bIsError);
	TSharedPtr<FJsonObject> BaselineGraphObj = GetGraphContractFirstGraph(BaselineResult);
	UNTEST_ASSERT_TRUE(BaselineGraphObj.IsValid());
	UNTEST_EXPECT_TRUE(GetGraphContractAnyPinHasDefaultValue(BaselineGraphObj));

	// include_pin_defaults=false -> no pin object anywhere carries default_value.
	TSharedPtr<FJsonObject> SuppressArgs = GetGraphContractMakeArgs(kGetGraphContractBPPath_PinDefaults);
	SuppressArgs->SetStringField(TEXT("node_detail_level"), TEXT("full"));
	SuppressArgs->SetBoolField(TEXT("include_pin_defaults"), false);
	IClaireonTool::FToolResult SuppressResult = Tool.Execute(SuppressArgs);
	UNTEST_ASSERT_FALSE(SuppressResult.bIsError);
	TSharedPtr<FJsonObject> SuppressGraphObj = GetGraphContractFirstGraph(SuppressResult);
	UNTEST_ASSERT_TRUE(SuppressGraphObj.IsValid());
	UNTEST_EXPECT_FALSE(GetGraphContractAnyPinHasDefaultValue(SuppressGraphObj));

	GetGraphContractCleanupAsset(kGetGraphContractBPPath_PinDefaults);
	co_return;
}

#endif // WITH_UNTESTED
