// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for claireon.bp_set_node_property.
//
// Creates a throwaway Blueprint under /Game/__MCPTests/, drops a node into the
// event graph via direct NewObject + AddNode, invokes the tool through its
// JSON Execute entry, and asserts on the resulting node state (property value,
// pin layout) plus error paths.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonBlueprintGraphTool_SetNodeProperty.h"
#include "Tools/IClaireonTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "K2Node_DynamicCast.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace
{
	static const TCHAR* TestBPPath_WriteAndReconstruct = TEXT("/Game/__MCPTests/BP_SetNodeProperty_WriteAndReconstruct");
	static const TCHAR* TestBPPath_ReconstructFalse    = TEXT("/Game/__MCPTests/BP_SetNodeProperty_ReconstructFalse");
	static const TCHAR* TestBPPath_BogusGuid           = TEXT("/Game/__MCPTests/BP_SetNodeProperty_BogusGuid");
	static const TCHAR* TestBPPath_UnknownProperty     = TEXT("/Game/__MCPTests/BP_SetNodeProperty_UnknownProperty");

	void CleanupSetNodePropTestAsset(const FString& AssetPath)
	{
		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad())
		{
			TArray<UObject*> AssetsToDelete;
			AssetsToDelete.Add(Asset);
			ObjectTools::ForceDeleteObjects(AssetsToDelete, false);
		}
	}

	UBlueprint* CreateSetNodePropTestActorBP(const FString& AssetPath)
	{
		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UBlueprint* Existing = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).TryLoad()))
		{
			return Existing;
		}

		UPackage* Package = CreatePackage(*AssetPath);
		if (!Package) return nullptr;

		const FString AssetName = FPackageName::GetShortName(AssetPath);
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);
		if (!BP) return nullptr;

		FAssetRegistryModule::AssetCreated(BP);
		BP->MarkPackageDirty();

		const FString PackageFileName = FPackageName::LongPackageNameToFilename(
			AssetPath, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		UPackage::Save(Package, BP, *PackageFileName, SaveArgs);

		return BP;
	}

	UEdGraph* FindSetNodePropTestEventGraph(UBlueprint* BP)
	{
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (G) return G;
		}
		return nullptr;
	}

	// Spawn a UK2Node_DynamicCast into the event graph and configure it to cast
	// to InitialTargetType. Returns the new node.
	UK2Node_DynamicCast* AddSetNodePropTestDynamicCast(UBlueprint* BP, UClass* InitialTargetType)
	{
		UEdGraph* EventGraph = FindSetNodePropTestEventGraph(BP);
		if (!EventGraph) return nullptr;

		UK2Node_DynamicCast* Node = NewObject<UK2Node_DynamicCast>(EventGraph);
		Node->TargetType = InitialTargetType;
		Node->SetFlags(RF_Transactional);
		EventGraph->AddNode(Node, /*bUserAction=*/false, /*bSelectNewNode=*/false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		return Node;
	}

	TSharedPtr<FJsonObject> MakeSetNodePropArgs(
		const TCHAR* AssetPath,
		const TCHAR* NodeGuid,
		const TCHAR* PropertyName,
		const TCHAR* PropertyValue,
		TOptional<bool> Reconstruct = TOptional<bool>())
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		if (AssetPath)      Args->SetStringField(TEXT("asset_path"), AssetPath);
		if (NodeGuid)       Args->SetStringField(TEXT("node_guid"), NodeGuid);
		if (PropertyName)   Args->SetStringField(TEXT("property_name"), PropertyName);
		if (PropertyValue)  Args->SetStringField(TEXT("property_value"), PropertyValue);
		if (Reconstruct.IsSet()) Args->SetBoolField(TEXT("reconstruct"), Reconstruct.GetValue());
		return Args;
	}
}

// ============================================================================
// Test 1: Writing TargetType on a K2Node_DynamicCast updates the field and
// ReconstructNode fires by default, propagating the change through the
// node's typed output pin.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, SetNodeProperty, Functional_WritesTargetTypeAndReconstructs, UNTEST_TIMEOUTMS(60000))
{
	CleanupSetNodePropTestAsset(TestBPPath_WriteAndReconstruct);
	UBlueprint* BP = CreateSetNodePropTestActorBP(TestBPPath_WriteAndReconstruct);
	UNTEST_ASSERT_PTR(BP);

	UK2Node_DynamicCast* Node = AddSetNodePropTestDynamicCast(BP, AActor::StaticClass());
	UNTEST_ASSERT_PTR(Node);
	UNTEST_ASSERT_EQ(Node->TargetType.Get(), AActor::StaticClass());

	const FString NodeGuidStr = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

	ClaireonBlueprintGraphTool_SetNodeProperty Tool;
	IClaireonTool::FToolResult R = Tool.Execute(
		MakeSetNodePropArgs(TestBPPath_WriteAndReconstruct, *NodeGuidStr, TEXT("TargetType"), TEXT("/Script/Engine.Pawn")));
	UNTEST_ASSERT_FALSE(R.bIsError);

	UNTEST_EXPECT_EQ(Node->TargetType.Get(), APawn::StaticClass());

	CleanupSetNodePropTestAsset(TestBPPath_WriteAndReconstruct);
	co_return;
}

// ============================================================================
// Test 2: reconstruct=false keeps the value write but suppresses ReconstructNode.
// Detect by leaving the node's TargetType updated but its pin set stale --
// AllocateDefaultPins below has already given us a baseline pin count, and the
// non-reconstructing path must not change it.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, SetNodeProperty, Functional_ReconstructFalseSkipsRebuild, UNTEST_TIMEOUTMS(60000))
{
	CleanupSetNodePropTestAsset(TestBPPath_ReconstructFalse);
	UBlueprint* BP = CreateSetNodePropTestActorBP(TestBPPath_ReconstructFalse);
	UNTEST_ASSERT_PTR(BP);

	UK2Node_DynamicCast* Node = AddSetNodePropTestDynamicCast(BP, AActor::StaticClass());
	UNTEST_ASSERT_PTR(Node);

	// Capture pin identities pre-mutation -- ReconstructNode would invalidate
	// these UEdGraphPin pointers (the node spawns fresh pin objects).
	TArray<UEdGraphPin*> PinsBefore = Node->Pins;
	const int32 PinCountBefore = PinsBefore.Num();

	const FString NodeGuidStr = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

	ClaireonBlueprintGraphTool_SetNodeProperty Tool;
	IClaireonTool::FToolResult R = Tool.Execute(
		MakeSetNodePropArgs(TestBPPath_ReconstructFalse, *NodeGuidStr, TEXT("TargetType"), TEXT("/Script/Engine.Pawn"), false));
	UNTEST_ASSERT_FALSE(R.bIsError);

	// Value write happened.
	UNTEST_EXPECT_EQ(Node->TargetType.Get(), APawn::StaticClass());

	// Same pin objects, same count -- no reconstruct.
	UNTEST_EXPECT_EQ(Node->Pins.Num(), PinCountBefore);
	for (int32 i = 0; i < PinCountBefore; ++i)
	{
		UNTEST_EXPECT_EQ(Node->Pins[i], PinsBefore[i]);
	}

	CleanupSetNodePropTestAsset(TestBPPath_ReconstructFalse);
	co_return;
}

// ============================================================================
// Test 3: An invalid node_guid surfaces an error rather than crashing or
// silently no-opping.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, SetNodeProperty, Errors_BogusNodeGuid, UNTEST_TIMEOUTMS(60000))
{
	CleanupSetNodePropTestAsset(TestBPPath_BogusGuid);
	UBlueprint* BP = CreateSetNodePropTestActorBP(TestBPPath_BogusGuid);
	UNTEST_ASSERT_PTR(BP);

	// Add at least one real node so the graph isn't empty -- exercises the
	// "node not found among present nodes" path rather than "empty graph" path.
	AddSetNodePropTestDynamicCast(BP, AActor::StaticClass());

	ClaireonBlueprintGraphTool_SetNodeProperty Tool;
	IClaireonTool::FToolResult R = Tool.Execute(
		MakeSetNodePropArgs(TestBPPath_BogusGuid, TEXT("AAAAAAAA-BBBB-CCCC-DDDD-EEEEEEEEEEEE"), TEXT("TargetType"), TEXT("/Script/Engine.Pawn")));
	UNTEST_EXPECT_TRUE(R.bIsError);

	CleanupSetNodePropTestAsset(TestBPPath_BogusGuid);
	co_return;
}

// ============================================================================
// Test 4: An invalid property name surfaces the error from WritePropertyByPath
// rather than crashing.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, SetNodeProperty, Errors_UnknownProperty, UNTEST_TIMEOUTMS(60000))
{
	CleanupSetNodePropTestAsset(TestBPPath_UnknownProperty);
	UBlueprint* BP = CreateSetNodePropTestActorBP(TestBPPath_UnknownProperty);
	UNTEST_ASSERT_PTR(BP);

	UK2Node_DynamicCast* Node = AddSetNodePropTestDynamicCast(BP, AActor::StaticClass());
	UNTEST_ASSERT_PTR(Node);

	const FString NodeGuidStr = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

	ClaireonBlueprintGraphTool_SetNodeProperty Tool;
	IClaireonTool::FToolResult R = Tool.Execute(
		MakeSetNodePropArgs(TestBPPath_UnknownProperty, *NodeGuidStr, TEXT("ThisPropertyDoesNotExist"), TEXT("anything")));
	UNTEST_EXPECT_TRUE(R.bIsError);

	// Value on the real field must not have been touched.
	UNTEST_EXPECT_EQ(Node->TargetType.Get(), AActor::StaticClass());

	CleanupSetNodePropTestAsset(TestBPPath_UnknownProperty);
	co_return;
}

#endif // WITH_UNTESTED
