// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Exercise changed-mode output through each snapshot consumer and real tool sessions.

#if WITH_UNTESTED

#include "Untest.h"

#include "ClaireonBlueprintHelpers.h"
#include "ClaireonSessionManager.h"
#include "Tools/ClaireonBPSnapshot.h"
#include "Tools/ClaireonBlueprintGraphTool_AddFunction.h"
#include "Tools/ClaireonBlueprintGraphTool_AddFunctionOverride.h"
#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/ClaireonBlueprintGraphTool_AddPin.h"
#include "Tools/ClaireonBlueprintGraphTool_Close.h"
#include "Tools/ClaireonBlueprintGraphTool_ConnectPins.h"
#include "Tools/ClaireonBlueprintGraphTool_DisconnectPin.h"
#include "Tools/ClaireonBlueprintGraphTool_MoveNode.h"
#include "Tools/ClaireonBlueprintGraphTool_Open.h"
#include "Tools/ClaireonBlueprintGraphTool_RecombinePin.h"
#include "Tools/ClaireonBlueprintGraphTool_RemoveNode.h"
#include "Tools/ClaireonBlueprintGraphTool_RemovePin.h"
#include "Tools/ClaireonBlueprintGraphTool_Save.h"
#include "Tools/ClaireonBlueprintGraphTool_SelectNode.h"
#include "Tools/ClaireonBlueprintGraphTool_SetNodeProperty.h"
#include "Tools/ClaireonBlueprintGraphTool_SetPinValue.h"
#include "Tools/ClaireonBlueprintGraphTool_SplitPin.h"
#include "Tools/IClaireonTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "PackageTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"

namespace ClaireonBPSnapMigTestsInternal
{

// Prefix helpers to avoid unity-build collisions.

/** Fixture assets live under /Game/__MCPTests, which is gitignored. */
FString BPSnapMig_Path(const TCHAR* Leaf)
{
	return FString(TEXT("/Game/__MCPTests/BP_BPSnapMig_")) + Leaf;
}

FString BPSnapMig_ObjectPath(const FString& AssetPath)
{
	return AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
}

/** Release any session on the asset, then force-delete it. Safe when neither exists. */
void BPSnapMig_CleanupAsset(const FString& AssetPath)
{
	FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);

	if (UObject* Asset = FSoftObjectPath(BPSnapMig_ObjectPath(AssetPath)).TryLoad(); IsValid(Asset))
	{
		TArray<UObject*> AssetsToDelete;
		AssetsToDelete.Add(Asset);
		ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
	}
}

/** A saved Actor Blueprint. Saved because the round-trip test needs a file to reload. */
UBlueprint* BPSnapMig_CreateActorBP(const FString& AssetPath)
{
	if (UBlueprint* Existing = Cast<UBlueprint>(
			FSoftObjectPath(BPSnapMig_ObjectPath(AssetPath)).TryLoad()); IsValid(Existing))
	{
		return Existing;
	}

	UPackage* Package = CreatePackage(*AssetPath);
	if (!IsValid(Package))
	{
		return nullptr;
	}

	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		Package,
		FName(*FPackageName::GetShortName(AssetPath)),
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None);
	if (!IsValid(BP))
	{
		return nullptr;
	}

	FAssetRegistryModule::AssetCreated(BP);
	BP->MarkPackageDirty();

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(
		AssetPath, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	UPackage::Save(Package, BP, *PackageFileName, SaveArgs);

	return BP;
}

UEdGraph* BPSnapMig_EventGraph(UBlueprint* BP)
{
	if (!IsValid(BP))
	{
		return nullptr;
	}
	for (UEdGraph* G : BP->UbergraphPages)
	{
		if (IsValid(G))
		{
			return G;
		}
	}
	return nullptr;
}

/** First node whose FullTitle contains Fragment. GUID as a hyphen-free string. */
FString BPSnapMig_FindNodeGuid(UEdGraph* Graph, const TCHAR* Fragment)
{
	if (!IsValid(Graph))
	{
		return FString();
	}
	for (UEdGraphNode* Node : Graph->Nodes)
	{
		if (IsValid(Node)
			&& Node->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Contains(Fragment))
		{
			return Node->NodeGuid.ToString();
		}
	}
	return FString();
}

/** Open a session and return its id. Empty on failure. */
FString BPSnapMig_OpenSession(const FString& AssetPath, const TCHAR* GraphName)
{
	ClaireonBlueprintGraphTool_Open Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	if (GraphName)
	{
		Args->SetStringField(TEXT("graph_name"), GraphName);
	}
	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError || !R.Data.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] bp_open failed: %s"), *R.ErrorMessage);
		return FString();
	}
	FString SessionId;
	R.Data->TryGetStringField(TEXT("session_id"), SessionId);
	return SessionId;
}

TSharedPtr<FJsonObject> BPSnapMig_Args(const FString& SessionId, const TCHAR* ResponseMode)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	if (ResponseMode)
	{
		Args->SetStringField(TEXT("response_mode"), ResponseMode);
	}
	return Args;
}

/** Add one CallFunction node, returning the created node's GUID string. */
FString BPSnapMig_AddCallFunction(
	const FString& SessionId,
	const TCHAR* FunctionName,
	const TCHAR* FunctionClass,
	int32 PosX,
	int32 PosY,
	const TCHAR* NodeClass = nullptr)
{
	ClaireonBlueprintGraphTool_AddNode Tool;
	TSharedPtr<FJsonObject> Args = BPSnapMig_Args(SessionId, TEXT("status"));
	Args->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	Args->SetStringField(TEXT("function_name"), FunctionName);
	Args->SetStringField(TEXT("function_class"), FunctionClass);
	Args->SetNumberField(TEXT("position_x"), PosX);
	Args->SetNumberField(TEXT("position_y"), PosY);
	Args->SetBoolField(TEXT("auto_connect_from_cursor"), false);
	if (NodeClass)
	{
		Args->SetStringField(TEXT("node_class"), NodeClass);
	}
	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError || !R.Data.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] bp_add_node(%s) failed: %s"), FunctionName, *R.ErrorMessage);
		return FString();
	}
	FString Guid;
	R.Data->TryGetStringField(TEXT("created_node_guid"), Guid);
	return Guid;
}

/** Connect two pins by node GUID. Returns the tool result so a caller can read the diff. */
IClaireonTool::FToolResult BPSnapMig_Connect(
	const FString& SessionId,
	const FString& SourceGuid,
	const TCHAR* SourcePin,
	const FString& TargetGuid,
	const TCHAR* TargetPin,
	const TCHAR* ResponseMode = TEXT("status"))
{
	ClaireonBlueprintGraphTool_ConnectPins Tool;
	TSharedPtr<FJsonObject> Args = BPSnapMig_Args(SessionId, ResponseMode);
	Args->SetStringField(TEXT("source_node_guid"), SourceGuid);
	Args->SetStringField(TEXT("source_pin_name"), SourcePin);
	Args->SetStringField(TEXT("target_node_guid"), TargetGuid);
	Args->SetStringField(TEXT("target_pin_name"), TargetPin);
	return Tool.Execute(Args);
}

void BPSnapMig_CloseSession(const FString& SessionId)
{
	if (SessionId.IsEmpty())
	{
		return;
	}
	ClaireonBlueprintGraphTool_Close Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("session_id"), SessionId);
	Tool.Execute(Args);
}

// Compare frozen title-keyed and current identity-keyed rendering on the same snapshots.

struct FBPSnapMigDiffCounts
{
	int32 Added = 0;
	int32 Removed = 0;
};

using FBPSnapMigFrozenMap = TMap<FGuid, TMap<FName, TArray<FString>>>;

/** FROZEN COPY of the retired title-keyed capture. Benchmark/differential reference only. */
void BPSnapMig_FrozenCapture(UEdGraph* Graph, FBPSnapMigFrozenMap& Out)
{
	Out.Empty();
	if (!IsValid(Graph))
	{
		return;
	}
	for (UEdGraphNode* SnapNode : Graph->Nodes)
	{
		if (!IsValid(SnapNode))
		{
			continue;
		}
		TMap<FName, TArray<FString>> PinConns;
		for (UEdGraphPin* SnapPin : SnapNode->Pins)
		{
			if (!SnapPin)
			{
				continue;
			}
			TArray<FString> ConnectedTo;
			for (UEdGraphPin* LinkedPin : SnapPin->LinkedTo)
			{
				if (LinkedPin && IsValid(LinkedPin->GetOwningNode()))
				{
					ConnectedTo.Add(LinkedPin->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
				}
			}
			PinConns.Add(SnapPin->PinName, ConnectedTo);
		}
		Out.Add(SnapNode->NodeGuid, PinConns);
	}
}

/** FROZEN COPY of the retired renderer's per-pin comparison, for one node. */
FBPSnapMigDiffCounts BPSnapMig_FrozenCompare(
	UEdGraphNode* LiveNode,
	const FBPSnapMigFrozenMap& PreOp)
{
	FBPSnapMigDiffCounts Counts;
	if (!IsValid(LiveNode))
	{
		return Counts;
	}
	const TMap<FName, TArray<FString>>* PrePinMap = PreOp.Find(LiveNode->NodeGuid);

	for (UEdGraphPin* DiffPin : LiveNode->Pins)
	{
		if (!DiffPin)
		{
			continue;
		}
		TArray<FString> CurrentConnected;
		for (UEdGraphPin* LinkedDiff : DiffPin->LinkedTo)
		{
			if (LinkedDiff && IsValid(LinkedDiff->GetOwningNode()))
			{
				CurrentConnected.Add(LinkedDiff->GetOwningNode()->GetNodeTitle(ENodeTitleType::FullTitle).ToString());
			}
		}
		TArray<FString> PreviousConnected;
		if (PrePinMap)
		{
			if (const TArray<FString>* PreConns = PrePinMap->Find(DiffPin->PinName))
			{
				PreviousConnected = *PreConns;
			}
		}
		for (const FString& CurConn : CurrentConnected)
		{
			if (!PreviousConnected.Contains(CurConn))
			{
				++Counts.Added;
			}
		}
		for (const FString& PrevConn : PreviousConnected)
		{
			if (!CurrentConnected.Contains(PrevConn))
			{
				++Counts.Removed;
			}
		}
	}
	return Counts;
}

/** The shipped identity-keyed comparison, for one node. Mirrors BuildStateResponse. */
FBPSnapMigDiffCounts BPSnapMig_IdentityCompare(
	UEdGraphNode* LiveNode,
	const FClaireonBPSnapshot& PreOp)
{
	FBPSnapMigDiffCounts Counts;
	if (!IsValid(LiveNode))
	{
		return Counts;
	}
	const FClaireonBPNodeSnapshot* PreNode = PreOp.FindNode(LiveNode->NodeGuid);

	for (UEdGraphPin* DiffPin : LiveNode->Pins)
	{
		if (!DiffPin)
		{
			continue;
		}
		TSet<FString> CurrentLinks;
		for (UEdGraphPin* LinkedDiff : DiffPin->LinkedTo)
		{
			if (!LinkedDiff || !IsValid(LinkedDiff->GetOwningNodeUnchecked()))
			{
				continue;
			}
			CurrentLinks.Add(FString::Printf(TEXT("%s/%s"),
				*LinkedDiff->GetOwningNodeUnchecked()->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
				*LinkedDiff->PinId.ToString(EGuidFormats::DigitsWithHyphens)));
		}
		TSet<FString> PreviousLinks;
		if (const FClaireonBPPinSnapshot* PrePin = PreNode ? PreNode->Pins.Find(DiffPin->PinId) : nullptr)
		{
			for (const FClaireonBPLinkEndpoint& Endpoint : PrePin->Links)
			{
				PreviousLinks.Add(FString::Printf(TEXT("%s/%s"),
					*Endpoint.NodeGuid.ToString(EGuidFormats::DigitsWithHyphens),
					*Endpoint.PinId.ToString(EGuidFormats::DigitsWithHyphens)));
			}
		}
		Counts.Added += CurrentLinks.Difference(PreviousLinks).Num();
		Counts.Removed += PreviousLinks.Difference(CurrentLinks).Num();
	}
	return Counts;
}

/** One UK2Node_CallFunction on Graph, pins allocated. */
UK2Node_CallFunction* BPSnapMig_MakeMathNode(UEdGraph* Graph, const TCHAR* FunctionName, int32 PosX)
{
	UK2Node_CallFunction* Node = NewObject<UK2Node_CallFunction>(Graph);
	Node->FunctionReference.SetExternalMember(FName(FunctionName), UKismetMathLibrary::StaticClass());
	Node->CreateNewGuid();
	Node->NodePosX = PosX;
	Graph->AddNode(Node, false, false);
	Node->PostPlacedNewNode();
	Node->AllocateDefaultPins();
	return Node;
}

} // namespace ClaireonBPSnapMigTestsInternal

// New function nodes report their wiring as added.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, Row01_AddFunction_Preserves, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	const FString AssetPath = BPSnapMig_Path(TEXT("Row01"));
	BPSnapMig_CleanupAsset(AssetPath);
	UNTEST_ASSERT_PTR(BPSnapMig_CreateActorBP(AssetPath));

	const FString SessionId = BPSnapMig_OpenSession(AssetPath, TEXT("EventGraph"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("name"), TEXT("Amount"));
	Input->SetStringField(TEXT("type"), TEXT("float"));
	TSharedPtr<FJsonObject> Output = MakeShared<FJsonObject>();
	Output->SetStringField(TEXT("name"), TEXT("Result"));
	Output->SetStringField(TEXT("type"), TEXT("float"));

	TArray<TSharedPtr<FJsonValue>> Inputs;
	Inputs.Add(MakeShared<FJsonValueObject>(Input));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(Output));

	ClaireonBlueprintGraphTool_AddFunction Tool;
	TSharedPtr<FJsonObject> Args = BPSnapMig_Args(SessionId, TEXT("changed"));
	Args->SetStringField(TEXT("function_name"), TEXT("BPSnapMigFn"));
	Args->SetArrayField(TEXT("inputs"), Inputs);
	Args->SetArrayField(TEXT("outputs"), Outputs);

	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] add_function failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("## Changed nodes (2 of 2)")));
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("ADDED:")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("REMOVED:")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("PIN REMOVED:")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("ORPHANED:")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("RENAMED:")));

	BPSnapMig_CloseSession(SessionId);
	BPSnapMig_CleanupAsset(AssetPath);
	co_return;
}

// New override nodes report their wiring as added.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, Row02_AddFunctionOverride_Preserves, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	const FString AssetPath = BPSnapMig_Path(TEXT("Row02"));
	BPSnapMig_CleanupAsset(AssetPath);
	UNTEST_ASSERT_PTR(BPSnapMig_CreateActorBP(AssetPath));

	const FString SessionId = BPSnapMig_OpenSession(AssetPath, TEXT("EventGraph"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	ClaireonBlueprintGraphTool_AddFunctionOverride Tool;
	TSharedPtr<FJsonObject> Args = BPSnapMig_Args(SessionId, TEXT("changed"));
	Args->SetStringField(TEXT("function_name"), TEXT("ReceiveEndPlay"));

	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] add_function_override failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("Event End Play")));
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("(exec connections unchanged)")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("REMOVED:")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("ORPHANED:")));

	BPSnapMig_CloseSession(SessionId);
	BPSnapMig_CleanupAsset(AssetPath);
	co_return;
}

// Auto-wiring must report the new link at both the new node and its existing neighbor.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, Row03_AddNode_AutoWiredNeighbourReportsAdded, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	const FString AssetPath = BPSnapMig_Path(TEXT("Row03"));
	BPSnapMig_CleanupAsset(AssetPath);
	UBlueprint* BP = BPSnapMig_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	const FString SessionId = BPSnapMig_OpenSession(AssetPath, TEXT("EventGraph"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString PrintGuid = BPSnapMig_AddCallFunction(
		SessionId, TEXT("PrintString"), TEXT("KismetSystemLibrary"), 400, 0);
	UNTEST_ASSERT_FALSE(PrintGuid.IsEmpty());

	// Put the cursor on the pre-existing node so the next add auto-wires to it.
	{
		ClaireonBlueprintGraphTool_SelectNode SelectTool;
		TSharedPtr<FJsonObject> SelectArgs = BPSnapMig_Args(SessionId, TEXT("status"));
		SelectArgs->SetStringField(TEXT("node_guid"), PrintGuid);
		const IClaireonTool::FToolResult SelectResult = SelectTool.Execute(SelectArgs);
		UNTEST_ASSERT_FALSE(SelectResult.bIsError);
	}

	ClaireonBlueprintGraphTool_AddNode Tool;
	TSharedPtr<FJsonObject> Args = BPSnapMig_Args(SessionId, TEXT("changed"));
	Args->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
	Args->SetStringField(TEXT("function_name"), TEXT("Delay"));
	Args->SetStringField(TEXT("function_class"), TEXT("KismetSystemLibrary"));
	Args->SetNumberField(TEXT("position_x"), 900);
	Args->SetNumberField(TEXT("position_y"), 0);
	Args->SetBoolField(TEXT("auto_connect_from_cursor"), true);

	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] add_node(Delay) failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("## Changed nodes (2 of")));
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("ADDED:   execute(exec) <- [Print String]")));
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("ADDED:   then(exec) -> [Delay]")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("REMOVED:")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("ORPHANED:")));

	BPSnapMig_CloseSession(SessionId);
	BPSnapMig_CleanupAsset(AssetPath);
	co_return;
}

// Adding a function-entry pin must preserve surviving PinIds and avoid spurious link changes.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, Row04_AddPin_EntryReconstructPreservesPinIdentity, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	const FString AssetPath = BPSnapMig_Path(TEXT("Row04"));
	BPSnapMig_CleanupAsset(AssetPath);
	UNTEST_ASSERT_PTR(BPSnapMig_CreateActorBP(AssetPath));

	FString SessionId = BPSnapMig_OpenSession(AssetPath, TEXT("EventGraph"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	// A function with one input and one output, then wire entry -> result through them.
	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("name"), TEXT("Amount"));
	Input->SetStringField(TEXT("type"), TEXT("float"));
	TSharedPtr<FJsonObject> Output = MakeShared<FJsonObject>();
	Output->SetStringField(TEXT("name"), TEXT("Result"));
	Output->SetStringField(TEXT("type"), TEXT("float"));
	TArray<TSharedPtr<FJsonValue>> Inputs;
	Inputs.Add(MakeShared<FJsonValueObject>(Input));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(Output));

	FString EntryGuid;
	FString ResultGuid;
	{
		ClaireonBlueprintGraphTool_AddFunction FnTool;
		TSharedPtr<FJsonObject> FnArgs = BPSnapMig_Args(SessionId, TEXT("status"));
		FnArgs->SetStringField(TEXT("function_name"), TEXT("BPSnapMigEntryFn"));
		FnArgs->SetArrayField(TEXT("inputs"), Inputs);
		FnArgs->SetArrayField(TEXT("outputs"), Outputs);
		const IClaireonTool::FToolResult FnResult = FnTool.Execute(FnArgs);
		if (FnResult.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] add_function failed: %s"), *FnResult.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(FnResult.bIsError);
	}

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(BPSnapMig_ObjectPath(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);
	UEdGraph* FnGraph = nullptr;
	for (UEdGraph* G : BP->FunctionGraphs)
	{
		if (IsValid(G) && G->GetName() == TEXT("BPSnapMigEntryFn"))
		{
			FnGraph = G;
		}
	}
	UNTEST_ASSERT_PTR(FnGraph);
	EntryGuid = BPSnapMig_FindNodeGuid(FnGraph, TEXT("BPSnapMigEntryFn"));
	ResultGuid = BPSnapMig_FindNodeGuid(FnGraph, TEXT("Return Node"));
	UNTEST_ASSERT_FALSE(EntryGuid.IsEmpty());
	UNTEST_ASSERT_FALSE(ResultGuid.IsEmpty());

	const IClaireonTool::FToolResult ConnectResult =
		BPSnapMig_Connect(SessionId, EntryGuid, TEXT("Amount"), ResultGuid, TEXT("Result"));
	if (ConnectResult.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] connect Amount->Result failed: %s"), *ConnectResult.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(ConnectResult.bIsError);

	ClaireonBlueprintGraphTool_AddPin Tool;
	TSharedPtr<FJsonObject> Args = BPSnapMig_Args(SessionId, TEXT("changed"));
	Args->SetStringField(TEXT("node_guid"), EntryGuid);
	Args->SetStringField(TEXT("pin_name"), TEXT("Extra"));
	Args->SetStringField(TEXT("pin_type"), TEXT("int"));

	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] add_pin failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("(exec connections unchanged)")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("PIN REMOVED:")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("ADDED:")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("REMOVED:")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("ORPHANED:")));

	BPSnapMig_CloseSession(SessionId);
	BPSnapMig_CleanupAsset(AssetPath);
	co_return;
}

// Distinguish two links to different pins on the same node.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, Row05_ConnectPins_SecondLinkToSameNodeReportsAdded, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	const FString AssetPath = BPSnapMig_Path(TEXT("Row05"));
	BPSnapMig_CleanupAsset(AssetPath);
	UBlueprint* BP = BPSnapMig_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	const FString SessionId = BPSnapMig_OpenSession(AssetPath, TEXT("EventGraph"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString AddGuid = BPSnapMig_AddCallFunction(
		SessionId, TEXT("Add_DoubleDouble"), TEXT("KismetMathLibrary"), 400, 0,
		TEXT("K2Node_PromotableOperator"));
	UNTEST_ASSERT_FALSE(AddGuid.IsEmpty());

	UEdGraph* Graph = BPSnapMig_EventGraph(BP);
	UNTEST_ASSERT_PTR(Graph);
	const FString TickGuid = BPSnapMig_FindNodeGuid(Graph, TEXT("Event Tick"));
	UNTEST_ASSERT_FALSE(TickGuid.IsEmpty());

	const IClaireonTool::FToolResult First =
		BPSnapMig_Connect(SessionId, TickGuid, TEXT("DeltaSeconds"), AddGuid, TEXT("A"), TEXT("changed"));
	UNTEST_ASSERT_FALSE(First.bIsError);
	UNTEST_EXPECT_TRUE(First.Summary.Contains(TEXT("ADDED:   DeltaSeconds")));

	const IClaireonTool::FToolResult Second =
		BPSnapMig_Connect(SessionId, TickGuid, TEXT("DeltaSeconds"), AddGuid, TEXT("B"), TEXT("changed"));
	if (Second.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] second connect failed: %s"), *Second.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(Second.bIsError);

	UNTEST_EXPECT_TRUE(Second.Summary.Contains(TEXT("ADDED:   DeltaSeconds")));
	UNTEST_EXPECT_TRUE(Second.Summary.Contains(TEXT("ADDED:   B(real) <- [Event Tick]")));

	BPSnapMig_CloseSession(SessionId);
	BPSnapMig_CleanupAsset(AssetPath);
	co_return;
}

// Report removal of one of two links to the same node.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, Row06_DisconnectPin_BreakingOneOfTwoLinksReportsRemoved, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	const FString AssetPath = BPSnapMig_Path(TEXT("Row06"));
	BPSnapMig_CleanupAsset(AssetPath);
	UBlueprint* BP = BPSnapMig_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	const FString SessionId = BPSnapMig_OpenSession(AssetPath, TEXT("EventGraph"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString AddGuid = BPSnapMig_AddCallFunction(
		SessionId, TEXT("Add_DoubleDouble"), TEXT("KismetMathLibrary"), 400, 0,
		TEXT("K2Node_PromotableOperator"));
	UNTEST_ASSERT_FALSE(AddGuid.IsEmpty());

	UEdGraph* Graph = BPSnapMig_EventGraph(BP);
	UNTEST_ASSERT_PTR(Graph);
	const FString TickGuid = BPSnapMig_FindNodeGuid(Graph, TEXT("Event Tick"));
	UNTEST_ASSERT_FALSE(TickGuid.IsEmpty());

	UNTEST_ASSERT_FALSE(BPSnapMig_Connect(SessionId, TickGuid, TEXT("DeltaSeconds"), AddGuid, TEXT("A")).bIsError);
	UNTEST_ASSERT_FALSE(BPSnapMig_Connect(SessionId, TickGuid, TEXT("DeltaSeconds"), AddGuid, TEXT("B")).bIsError);

	ClaireonBlueprintGraphTool_DisconnectPin Tool;
	TSharedPtr<FJsonObject> Args = BPSnapMig_Args(SessionId, TEXT("changed"));
	Args->SetStringField(TEXT("node_guid"), AddGuid);
	Args->SetStringField(TEXT("pin_name"), TEXT("A"));

	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] disconnect_pin failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("REMOVED: A(real) <- [Event Tick] (now unconnected)")));
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("REMOVED: DeltaSeconds(real) -> [float + float]")));

	BPSnapMig_CloseSession(SessionId);
	BPSnapMig_CleanupAsset(AssetPath);
	co_return;
}

// Moving a node does not change its wiring.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, Row07_MoveNode_Preserves, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	const FString AssetPath = BPSnapMig_Path(TEXT("Row07"));
	BPSnapMig_CleanupAsset(AssetPath);
	UNTEST_ASSERT_PTR(BPSnapMig_CreateActorBP(AssetPath));

	const FString SessionId = BPSnapMig_OpenSession(AssetPath, TEXT("EventGraph"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString PrintGuid = BPSnapMig_AddCallFunction(
		SessionId, TEXT("PrintString"), TEXT("KismetSystemLibrary"), 400, 0);
	UNTEST_ASSERT_FALSE(PrintGuid.IsEmpty());

	ClaireonBlueprintGraphTool_MoveNode Tool;
	TSharedPtr<FJsonObject> Args = BPSnapMig_Args(SessionId, TEXT("changed"));
	Args->SetStringField(TEXT("node_guid"), PrintGuid);
	Args->SetNumberField(TEXT("position_x"), 512);
	Args->SetNumberField(TEXT("position_y"), 64);

	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);

	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("(exec connections unchanged)")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("ADDED:")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("REMOVED:")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("ORPHANED:")));

	BPSnapMig_CloseSession(SessionId);
	BPSnapMig_CleanupAsset(AssetPath);
	co_return;
}

// Splitting preserves the parent pin identity; recombining reports removed subpins.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, Row13SplitThenRow08Recombine_SubPinsReportedAsRemoved, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	const FString AssetPath = BPSnapMig_Path(TEXT("Row0813"));
	BPSnapMig_CleanupAsset(AssetPath);
	UNTEST_ASSERT_PTR(BPSnapMig_CreateActorBP(AssetPath));

	const FString SessionId = BPSnapMig_OpenSession(AssetPath, TEXT("EventGraph"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString SetLocGuid = BPSnapMig_AddCallFunction(
		SessionId, TEXT("K2_SetActorLocation"), TEXT("Actor"), 400, 400);
	UNTEST_ASSERT_FALSE(SetLocGuid.IsEmpty());

	{
		ClaireonBlueprintGraphTool_SplitPin SplitTool;
		TSharedPtr<FJsonObject> SplitArgs = BPSnapMig_Args(SessionId, TEXT("changed"));
		SplitArgs->SetStringField(TEXT("node_guid"), SetLocGuid);
		SplitArgs->SetStringField(TEXT("pin_name"), TEXT("NewLocation"));
		const IClaireonTool::FToolResult SplitResult = SplitTool.Execute(SplitArgs);
		if (SplitResult.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] split_pin failed: %s"), *SplitResult.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(SplitResult.bIsError);
		UNTEST_EXPECT_TRUE(SplitResult.Summary.Contains(TEXT("(exec connections unchanged)")));
		UNTEST_EXPECT_FALSE(SplitResult.Summary.Contains(TEXT("PIN REMOVED:")));
		UNTEST_EXPECT_FALSE(SplitResult.Summary.Contains(TEXT("ORPHANED:")));
	}

	{
		ClaireonBlueprintGraphTool_RecombinePin RecombineTool;
		TSharedPtr<FJsonObject> RecombineArgs = BPSnapMig_Args(SessionId, TEXT("changed"));
		RecombineArgs->SetStringField(TEXT("node_guid"), SetLocGuid);
		RecombineArgs->SetStringField(TEXT("pin_name"), TEXT("NewLocation"));
		const IClaireonTool::FToolResult RecombineResult = RecombineTool.Execute(RecombineArgs);
		if (RecombineResult.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] recombine_pin failed: %s"), *RecombineResult.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(RecombineResult.bIsError);
		UNTEST_EXPECT_TRUE(RecombineResult.Summary.Contains(TEXT("PIN REMOVED: NewLocation_X")));
		UNTEST_EXPECT_TRUE(RecombineResult.Summary.Contains(TEXT("PIN REMOVED: NewLocation_Y")));
		UNTEST_EXPECT_TRUE(RecombineResult.Summary.Contains(TEXT("PIN REMOVED: NewLocation_Z")));
	}

	BPSnapMig_CloseSession(SessionId);
	BPSnapMig_CleanupAsset(AssetPath);
	co_return;
}

// Deleting one of two same-title nodes must report its broken link and identify the deleted endpoint.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, Row09_RemoveNode_ReportsBrokenLinkWithDeletedFarEndLabel, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	const FString AssetPath = BPSnapMig_Path(TEXT("Row09"));
	BPSnapMig_CleanupAsset(AssetPath);
	UBlueprint* BP = BPSnapMig_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	const FString SessionId = BPSnapMig_OpenSession(AssetPath, TEXT("EventGraph"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString AbsA = BPSnapMig_AddCallFunction(SessionId, TEXT("Abs"), TEXT("KismetMathLibrary"), 400, 0);
	const FString AbsB = BPSnapMig_AddCallFunction(SessionId, TEXT("Abs"), TEXT("KismetMathLibrary"), 400, 200);
	UNTEST_ASSERT_FALSE(AbsA.IsEmpty());
	UNTEST_ASSERT_FALSE(AbsB.IsEmpty());

	UEdGraph* Graph = BPSnapMig_EventGraph(BP);
	UNTEST_ASSERT_PTR(Graph);
	const FString TickGuid = BPSnapMig_FindNodeGuid(Graph, TEXT("Event Tick"));
	UNTEST_ASSERT_FALSE(TickGuid.IsEmpty());

	UNTEST_ASSERT_FALSE(BPSnapMig_Connect(SessionId, TickGuid, TEXT("DeltaSeconds"), AbsA, TEXT("A")).bIsError);
	UNTEST_ASSERT_FALSE(BPSnapMig_Connect(SessionId, TickGuid, TEXT("DeltaSeconds"), AbsB, TEXT("A")).bIsError);

	ClaireonBlueprintGraphTool_RemoveNode Tool;
	TSharedPtr<FJsonObject> Args = BPSnapMig_Args(SessionId, TEXT("changed"));
	Args->SetStringField(TEXT("node_guid"), AbsA);

	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] remove_node failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("REMOVED: DeltaSeconds(real) ->")));
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("[deleted K2Node_CallFunction [GUID: ")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("(now unconnected)")));

	BPSnapMig_CloseSession(SessionId);
	BPSnapMig_CleanupAsset(AssetPath);
	co_return;
}

// Removing a middle operator pin destroys one PinId and renames another in place; report both.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, Row10_RemovePin_ReportsRenameAndVanishedPin, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	const FString AssetPath = BPSnapMig_Path(TEXT("Row10"));
	BPSnapMig_CleanupAsset(AssetPath);
	UBlueprint* BP = BPSnapMig_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	const FString SessionId = BPSnapMig_OpenSession(AssetPath, TEXT("EventGraph"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString AddGuid = BPSnapMig_AddCallFunction(
		SessionId, TEXT("Add_DoubleDouble"), TEXT("KismetMathLibrary"), 400, 0,
		TEXT("K2Node_PromotableOperator"));
	UNTEST_ASSERT_FALSE(AddGuid.IsEmpty());

	{
		ClaireonBlueprintGraphTool_AddPin AddPinTool;
		TSharedPtr<FJsonObject> AddPinArgs = BPSnapMig_Args(SessionId, TEXT("status"));
		AddPinArgs->SetStringField(TEXT("node_guid"), AddGuid);
		const IClaireonTool::FToolResult AddPinResult = AddPinTool.Execute(AddPinArgs);
		if (AddPinResult.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] add_pin failed: %s"), *AddPinResult.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(AddPinResult.bIsError);
	}

	UEdGraph* Graph = BPSnapMig_EventGraph(BP);
	UNTEST_ASSERT_PTR(Graph);
	const FString TickGuid = BPSnapMig_FindNodeGuid(Graph, TEXT("Event Tick"));
	UNTEST_ASSERT_FALSE(TickGuid.IsEmpty());

	UNTEST_ASSERT_FALSE(BPSnapMig_Connect(SessionId, TickGuid, TEXT("DeltaSeconds"), AddGuid, TEXT("B")).bIsError);
	UNTEST_ASSERT_FALSE(BPSnapMig_Connect(SessionId, TickGuid, TEXT("DeltaSeconds"), AddGuid, TEXT("C")).bIsError);

	ClaireonBlueprintGraphTool_RemovePin Tool;
	TSharedPtr<FJsonObject> Args = BPSnapMig_Args(SessionId, TEXT("changed"));
	Args->SetStringField(TEXT("node_guid"), AddGuid);
	Args->SetStringField(TEXT("pin_name"), TEXT("B"));

	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] remove_pin failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("RENAMED: C -> B(real)")));
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("PIN REMOVED: B <- [Event Tick]")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("(exec connections unchanged)")));

	BPSnapMig_CloseSession(SessionId);
	BPSnapMig_CleanupAsset(AssetPath);
	co_return;
}

// Retargeting a connected cast must report the old result pin becoming orphaned even when its links remain.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, Row11_SetNodeProperty_ReportsOrphanedPin, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	const FString AssetPath = BPSnapMig_Path(TEXT("Row11"));
	BPSnapMig_CleanupAsset(AssetPath);
	UBlueprint* BP = BPSnapMig_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	const FString SessionId = BPSnapMig_OpenSession(AssetPath, TEXT("EventGraph"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	FString CastGuid;
	{
		ClaireonBlueprintGraphTool_AddNode CastTool;
		TSharedPtr<FJsonObject> CastArgs = BPSnapMig_Args(SessionId, TEXT("status"));
		CastArgs->SetStringField(TEXT("node_type"), TEXT("Cast"));
		CastArgs->SetStringField(TEXT("target_class"), TEXT("Pawn"));
		CastArgs->SetNumberField(TEXT("position_x"), 400);
		CastArgs->SetNumberField(TEXT("position_y"), 0);
		CastArgs->SetBoolField(TEXT("auto_connect_from_cursor"), false);
		const IClaireonTool::FToolResult CastResult = CastTool.Execute(CastArgs);
		if (CastResult.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] add Cast node failed: %s"), *CastResult.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(CastResult.bIsError);
		UNTEST_ASSERT_TRUE(CastResult.Data.IsValid());
		CastResult.Data->TryGetStringField(TEXT("created_node_guid"), CastGuid);
	}
	UNTEST_ASSERT_FALSE(CastGuid.IsEmpty());

	const FString IsValidGuid = BPSnapMig_AddCallFunction(
		SessionId, TEXT("IsValid"), TEXT("KismetSystemLibrary"), 900, 0);
	UNTEST_ASSERT_FALSE(IsValidGuid.IsEmpty());

	UNTEST_ASSERT_FALSE(
		BPSnapMig_Connect(SessionId, CastGuid, TEXT("AsPawn"), IsValidGuid, TEXT("Object")).bIsError);

	ClaireonBlueprintGraphTool_SetNodeProperty Tool;
	TSharedPtr<FJsonObject> Args = BPSnapMig_Args(SessionId, TEXT("changed"));
	Args->SetStringField(TEXT("node_guid"), CastGuid);
	Args->SetStringField(TEXT("property_name"), TEXT("TargetType"));
	Args->SetStringField(TEXT("property_value"), TEXT("Actor"));
	Args->SetBoolField(TEXT("reconstruct"), true);

	const IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] set_node_property failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("ORPHANED: AsPawn")));
	UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("no longer matched by the reconstructed node")));
	UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("(exec connections unchanged)")));

	BPSnapMig_CloseSession(SessionId);
	BPSnapMig_CleanupAsset(AssetPath);
	co_return;
}

// Connected-pin defaults are refused; unconnected default changes do not alter changed-mode wiring output.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, Row12_SetPinValue_RefusesConnectedPinAndPreserves, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	const FString AssetPath = BPSnapMig_Path(TEXT("Row12"));
	BPSnapMig_CleanupAsset(AssetPath);
	UBlueprint* BP = BPSnapMig_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	const FString SessionId = BPSnapMig_OpenSession(AssetPath, TEXT("EventGraph"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString DelayGuid = BPSnapMig_AddCallFunction(
		SessionId, TEXT("Delay"), TEXT("KismetSystemLibrary"), 400, 0);
	UNTEST_ASSERT_FALSE(DelayGuid.IsEmpty());

	UEdGraph* Graph = BPSnapMig_EventGraph(BP);
	UNTEST_ASSERT_PTR(Graph);
	const FString TickGuid = BPSnapMig_FindNodeGuid(Graph, TEXT("Event Tick"));
	UNTEST_ASSERT_FALSE(TickGuid.IsEmpty());

	UNTEST_ASSERT_FALSE(
		BPSnapMig_Connect(SessionId, TickGuid, TEXT("DeltaSeconds"), DelayGuid, TEXT("Duration")).bIsError);

	{
		ClaireonBlueprintGraphTool_SetPinValue ConnectedTool;
		TSharedPtr<FJsonObject> ConnectedArgs = BPSnapMig_Args(SessionId, TEXT("changed"));
		ConnectedArgs->SetStringField(TEXT("node_guid"), DelayGuid);
		ConnectedArgs->SetStringField(TEXT("pin_name"), TEXT("Duration"));
		ConnectedArgs->SetStringField(TEXT("value"), TEXT("2.5"));
		const IClaireonTool::FToolResult ConnectedResult = ConnectedTool.Execute(ConnectedArgs);
		UNTEST_EXPECT_TRUE(ConnectedResult.bIsError);
		UNTEST_EXPECT_TRUE(ConnectedResult.ErrorMessage.Contains(TEXT("connected pin")));
	}

	{
		ClaireonBlueprintGraphTool_DisconnectPin DisconnectTool;
		TSharedPtr<FJsonObject> DisconnectArgs = BPSnapMig_Args(SessionId, TEXT("status"));
		DisconnectArgs->SetStringField(TEXT("node_guid"), DelayGuid);
		DisconnectArgs->SetStringField(TEXT("pin_name"), TEXT("Duration"));
		UNTEST_ASSERT_FALSE(DisconnectTool.Execute(DisconnectArgs).bIsError);
	}

	{
		ClaireonBlueprintGraphTool_SetPinValue Tool;
		TSharedPtr<FJsonObject> Args = BPSnapMig_Args(SessionId, TEXT("changed"));
		Args->SetStringField(TEXT("node_guid"), DelayGuid);
		Args->SetStringField(TEXT("pin_name"), TEXT("Duration"));
		Args->SetStringField(TEXT("value"), TEXT("2.5"));

		const IClaireonTool::FToolResult R = Tool.Execute(Args);
		if (R.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] set_pin_value failed: %s"), *R.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(R.bIsError);
		UNTEST_EXPECT_TRUE(R.Summary.Contains(TEXT("(exec connections unchanged)")));
		UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("REMOVED:")));
		UNTEST_EXPECT_FALSE(R.Summary.Contains(TEXT("ORPHANED:")));
	}

	BPSnapMig_CloseSession(SessionId);
	BPSnapMig_CleanupAsset(AssetPath);
	co_return;
}

// Compare identity and title keys for same-title endpoints, multiple target pins, and neighbor renames.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, Row14_Mechanism_TitleKeyingMissesWhatIdentityKeyingCatches, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	// Use a unique name for repeated runs in one process.
	const FName FixtureName = MakeUniqueObjectName(
		GetTransientPackage(), UBlueprint::StaticClass(), TEXT("BPSnapMig_MechanismFixture"));

	UBlueprint* Blueprint = FKismetEditorUtilities::CreateBlueprint(
		AActor::StaticClass(),
		GetTransientPackage(),
		FixtureName,
		BPTYPE_Normal,
		UBlueprint::StaticClass(),
		UBlueprintGeneratedClass::StaticClass(),
		NAME_None);
	UNTEST_ASSERT_PTR(Blueprint);
	UNTEST_ASSERT_TRUE(Blueprint->UbergraphPages.Num() > 0);

	UEdGraph* Graph = Blueprint->UbergraphPages[0];
	UNTEST_ASSERT_PTR(Graph);

	TArray<UEdGraph*> Graphs;
	Graphs.Add(Graph);

	// Source, and two identically-titled sinks.
	UK2Node_CallFunction* Source = BPSnapMig_MakeMathNode(Graph, TEXT("Add_IntInt"), 0);
	UK2Node_CallFunction* SinkOne = BPSnapMig_MakeMathNode(Graph, TEXT("Add_IntInt"), 400);
	UK2Node_CallFunction* SinkTwo = BPSnapMig_MakeMathNode(Graph, TEXT("Add_IntInt"), 800);
	UNTEST_ASSERT_PTR(Source);
	UNTEST_ASSERT_PTR(SinkOne);
	UNTEST_ASSERT_PTR(SinkTwo);

	UEdGraphPin* SourceOut = Source->FindPin(TEXT("ReturnValue"), EGPD_Output);
	UEdGraphPin* SinkOneA = SinkOne->FindPin(TEXT("A"), EGPD_Input);
	UEdGraphPin* SinkTwoA = SinkTwo->FindPin(TEXT("A"), EGPD_Input);
	UEdGraphPin* SinkTwoB = SinkTwo->FindPin(TEXT("B"), EGPD_Input);
	UNTEST_ASSERT_PTR(SourceOut);
	UNTEST_ASSERT_PTR(SinkOneA);
	UNTEST_ASSERT_PTR(SinkTwoA);
	UNTEST_ASSERT_PTR(SinkTwoB);

	UNTEST_EXPECT_TRUE(SinkOne->GetNodeTitle(ENodeTitleType::FullTitle).ToString()
		== SinkTwo->GetNodeTitle(ENodeTitleType::FullTitle).ToString());

	// --- C1: two identically-titled nodes on one pin; break one link ---------
	SourceOut->MakeLinkTo(SinkOneA);
	SourceOut->MakeLinkTo(SinkTwoA);

	FBPSnapMigFrozenMap FrozenBefore;
	BPSnapMig_FrozenCapture(Graph, FrozenBefore);
	FClaireonBPSnapshot SnapshotBefore;
	UNTEST_ASSERT_TRUE(ClaireonBPSnapshot::CaptureForChangedDiff(Graphs, SnapshotBefore));

	SourceOut->BreakLinkTo(SinkOneA);

	const FBPSnapMigDiffCounts FrozenC1 = BPSnapMig_FrozenCompare(Source, FrozenBefore);
	const FBPSnapMigDiffCounts IdentityC1 = BPSnapMig_IdentityCompare(Source, SnapshotBefore);
	UNTEST_EXPECT_EQ(FrozenC1.Removed, 0);
	UNTEST_EXPECT_EQ(IdentityC1.Removed, 1);
	UNTEST_EXPECT_EQ(IdentityC1.Added, 0);

	// --- C3: two links from one pin to two pins on the SAME node -------------
	SourceOut->BreakAllPinLinks();
	SourceOut->MakeLinkTo(SinkTwoA);

	FBPSnapMigFrozenMap FrozenBeforeC3;
	BPSnapMig_FrozenCapture(Graph, FrozenBeforeC3);
	FClaireonBPSnapshot SnapshotBeforeC3;
	UNTEST_ASSERT_TRUE(ClaireonBPSnapshot::CaptureForChangedDiff(Graphs, SnapshotBeforeC3));

	SourceOut->MakeLinkTo(SinkTwoB);

	const FBPSnapMigDiffCounts FrozenC3 = BPSnapMig_FrozenCompare(Source, FrozenBeforeC3);
	const FBPSnapMigDiffCounts IdentityC3 = BPSnapMig_IdentityCompare(Source, SnapshotBeforeC3);
	UNTEST_EXPECT_EQ(FrozenC3.Added, 0);
	UNTEST_EXPECT_EQ(IdentityC3.Added, 1);
	UNTEST_EXPECT_EQ(IdentityC3.Removed, 0);

	// --- C2: rename a CONNECTED node; no wiring moves ------------------------
	FBPSnapMigFrozenMap FrozenBeforeC2;
	BPSnapMig_FrozenCapture(Graph, FrozenBeforeC2);
	FClaireonBPSnapshot SnapshotBeforeC2;
	UNTEST_ASSERT_TRUE(ClaireonBPSnapshot::CaptureForChangedDiff(Graphs, SnapshotBeforeC2));

	const FString TitleBefore = SinkTwo->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	// Keep pin names and links while changing the node title.
	SinkTwo->FunctionReference.SetExternalMember(
		FName(TEXT("Subtract_IntInt")), UKismetMathLibrary::StaticClass());
	SinkTwo->ReconstructNode();
	const FString TitleAfter = SinkTwo->GetNodeTitle(ENodeTitleType::FullTitle).ToString();
	UNTEST_ASSERT_TRUE(TitleBefore != TitleAfter);

	const FBPSnapMigDiffCounts FrozenC2 = BPSnapMig_FrozenCompare(Source, FrozenBeforeC2);
	const FBPSnapMigDiffCounts IdentityC2 = BPSnapMig_IdentityCompare(Source, SnapshotBeforeC2);
	UNTEST_EXPECT_GT(FrozenC2.Added, 0);
	UNTEST_EXPECT_EQ(FrozenC2.Added, FrozenC2.Removed);
	UNTEST_EXPECT_EQ(IdentityC2.Added, 0);
	UNTEST_EXPECT_EQ(IdentityC2.Removed, 0);

	co_return;
}

// Save, unload, and reload; inspect persisted node and wire state.
UNTEST_UNIT_OPTS(Claireon, BPSnapshotMigration, RoundTrip_MutationSurvivesSaveUnloadReload, UNTEST_TIMEOUTMS(180000))
{
	using namespace ClaireonBPSnapMigTestsInternal;

	const FString AssetPath = BPSnapMig_Path(TEXT("RoundTrip"));
	BPSnapMig_CleanupAsset(AssetPath);
	UBlueprint* BP = BPSnapMig_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	const FString SessionId = BPSnapMig_OpenSession(AssetPath, TEXT("EventGraph"));
	UNTEST_ASSERT_FALSE(SessionId.IsEmpty());

	const FString AddGuid = BPSnapMig_AddCallFunction(
		SessionId, TEXT("Add_DoubleDouble"), TEXT("KismetMathLibrary"), 400, 0,
		TEXT("K2Node_PromotableOperator"));
	UNTEST_ASSERT_FALSE(AddGuid.IsEmpty());

	UEdGraph* Graph = BPSnapMig_EventGraph(BP);
	UNTEST_ASSERT_PTR(Graph);
	const FString TickGuid = BPSnapMig_FindNodeGuid(Graph, TEXT("Event Tick"));
	UNTEST_ASSERT_FALSE(TickGuid.IsEmpty());

	const IClaireonTool::FToolResult ConnectResult =
		BPSnapMig_Connect(SessionId, TickGuid, TEXT("DeltaSeconds"), AddGuid, TEXT("A"), TEXT("changed"));
	UNTEST_ASSERT_FALSE(ConnectResult.bIsError);
	UNTEST_EXPECT_TRUE(ConnectResult.Summary.Contains(TEXT("ADDED:")));

	{
		ClaireonBlueprintGraphTool_Save SaveTool;
		TSharedPtr<FJsonObject> SaveArgs = MakeShared<FJsonObject>();
		SaveArgs->SetStringField(TEXT("session_id"), SessionId);
		const IClaireonTool::FToolResult SaveResult = SaveTool.Execute(SaveArgs);
		if (SaveResult.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] bp_save failed: %s"), *SaveResult.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(SaveResult.bIsError);
	}

	BPSnapMig_CloseSession(SessionId);
	FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);

	FGuid PreReloadNodeGuid;
	UNTEST_ASSERT_TRUE(FGuid::Parse(AddGuid, PreReloadNodeGuid));

	if (UPackage* Package = FindPackage(nullptr, *AssetPath); IsValid(Package))
	{
		FText UnloadError;
		const bool bUnloaded = UPackageTools::UnloadPackages(
			{ Package }, UnloadError, /*bUnloadDirtyPackages=*/true);
		if (!bUnloaded)
		{
			UE_LOG(LogTemp, Error, TEXT("[BPSnapMig] UnloadPackages failed: %s"), *UnloadError.ToString());
		}
		UNTEST_ASSERT_TRUE(bUnloaded);
	}
	UNTEST_ASSERT_TRUE(FindPackage(nullptr, *AssetPath) == nullptr);

	UBlueprint* Reloaded = LoadObject<UBlueprint>(nullptr, *BPSnapMig_ObjectPath(AssetPath));
	UNTEST_ASSERT_PTR(Reloaded);
	UEdGraph* ReloadedGraph = BPSnapMig_EventGraph(Reloaded);
	UNTEST_ASSERT_PTR(ReloadedGraph);

	UEdGraphNode* ReloadedNode = nullptr;
	for (UEdGraphNode* Node : ReloadedGraph->Nodes)
	{
		if (IsValid(Node) && Node->NodeGuid == PreReloadNodeGuid)
		{
			ReloadedNode = Node;
		}
	}
	UNTEST_ASSERT_PTR(ReloadedNode);

	UEdGraphPin* ReloadedAPin = ReloadedNode->FindPin(TEXT("A"), EGPD_Input);
	UNTEST_ASSERT_PTR(ReloadedAPin);
	UNTEST_EXPECT_EQ(ReloadedAPin->LinkedTo.Num(), 1);
	if (ReloadedAPin->LinkedTo.Num() == 1 && ReloadedAPin->LinkedTo[0])
	{
		UEdGraphNode* FarNode = ReloadedAPin->LinkedTo[0]->GetOwningNodeUnchecked();
		UNTEST_EXPECT_TRUE(IsValid(FarNode)
			&& FarNode->GetNodeTitle(ENodeTitleType::FullTitle).ToString().Contains(TEXT("Event Tick")));
	}

	TArray<UEdGraph*> ReloadedGraphs;
	ReloadedGraphs.Add(ReloadedGraph);
	FClaireonBPSnapshot ReloadedSnapshot;
	UNTEST_ASSERT_TRUE(ClaireonBPSnapshot::CaptureForChangedDiff(ReloadedGraphs, ReloadedSnapshot));
	const FClaireonBPNodeSnapshot* ReloadedNodeSnapshot = ReloadedSnapshot.FindNode(PreReloadNodeGuid);
	UNTEST_ASSERT_TRUE(ReloadedNodeSnapshot != nullptr);
	const FClaireonBPPinSnapshot* ReloadedPinSnapshot = ReloadedNodeSnapshot->Pins.Find(ReloadedAPin->PinId);
	UNTEST_ASSERT_TRUE(ReloadedPinSnapshot != nullptr);
	UNTEST_EXPECT_EQ(ReloadedPinSnapshot->Links.Num(), 1);

	BPSnapMig_CleanupAsset(AssetPath);
	co_return;
}

#endif // WITH_UNTESTED
