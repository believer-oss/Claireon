// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Parameter-contract tests for bp_add_node / bp_move_node / bp_add_component
// (WI-1 of the 2026-07-05 defect fan-out).
//
// Each test creates a throwaway Blueprint under /Game/__MCPTests/, drives the
// tool through its JSON Execute entry point, and asserts on the resulting
// graph/SCS state plus exact error text on failure paths. Assets and their
// auto-opened sessions are released in per-test cleanup.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonBlueprintGraphTool_AddComponent.h"
#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/ClaireonBlueprintGraphTool_MoveNode.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonSessionManager.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "GameFramework/Actor.h"
#include "K2Node_AssignDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Event.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_SpawnActorFromClass.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonGraphToolParamContractTests_anon
{
	static const TCHAR* GTPC_Path_AddNodeScalarPos = TEXT("/Game/__MCPTests/BP_ParamContract_AddNodeScalarPos");
	static const TCHAR* GTPC_Path_AddNodeObjectPos = TEXT("/Game/__MCPTests/BP_ParamContract_AddNodeObjectPos");
	static const TCHAR* GTPC_Path_MoveNodeForms    = TEXT("/Game/__MCPTests/BP_ParamContract_MoveNodeForms");
	static const TCHAR* GTPC_Path_MoveNodePartial  = TEXT("/Game/__MCPTests/BP_ParamContract_MoveNodePartial");
	static const TCHAR* GTPC_Path_AddComponent     = TEXT("/Game/__MCPTests/BP_ParamContract_AddComponent");
	static const TCHAR* GTPC_Path_CBEMissingField  = TEXT("/Game/__MCPTests/BP_ParamContract_CBEMissingField");
	static const TCHAR* GTPC_Path_SpawnActorClass  = TEXT("/Game/__MCPTests/BP_ParamContract_SpawnActorClass");
	static const TCHAR* GTPC_Path_AssignDelegate   = TEXT("/Game/__MCPTests/BP_ParamContract_AssignDelegate");

	// Release any session auto-opened on the asset, then force-delete it.
	static void GTPC_CleanupAsset(const FString& AssetPath)
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

	static UBlueprint* GTPC_CreateActorBP(const FString& AssetPath)
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

	static UEdGraph* GTPC_FindEventGraph(UBlueprint* BP)
	{
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (IsValid(G)) return G;
		}
		return nullptr;
	}

	// First Branch (UK2Node_IfThenElse) node in the event graph, or null.
	static UK2Node_IfThenElse* GTPC_FindBranchNode(UBlueprint* BP)
	{
		UEdGraph* EventGraph = GTPC_FindEventGraph(BP);
		if (!IsValid(EventGraph)) return nullptr;
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			if (UK2Node_IfThenElse* AsBranch = Cast<UK2Node_IfThenElse>(Node); IsValid(AsBranch))
			{
				return AsBranch;
			}
		}
		return nullptr;
	}

	// Direct (non-tool) node placement for move_node tests, mirroring the
	// SetNodeProperty test fixture's spawn sequence.
	static UK2Node_IfThenElse* GTPC_AddBranchNodeDirect(UBlueprint* BP)
	{
		UEdGraph* EventGraph = GTPC_FindEventGraph(BP);
		if (!IsValid(EventGraph)) return nullptr;

		UK2Node_IfThenElse* Node = NewObject<UK2Node_IfThenElse>(EventGraph);
		Node->SetFlags(RF_Transactional);
		EventGraph->AddNode(Node, /*bUserAction=*/false, /*bSelectNewNode=*/false);
		Node->CreateNewGuid();
		Node->PostPlacedNewNode();
		Node->AllocateDefaultPins();
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		return Node;
	}

	static TSharedPtr<FJsonObject> GTPC_MakePositionObject(double X, double Y)
	{
		TSharedPtr<FJsonObject> Pos = MakeShared<FJsonObject>();
		Pos->SetNumberField(TEXT("x"), X);
		Pos->SetNumberField(TEXT("y"), Y);
		return Pos;
	}

	// Number of event-graph nodes of exactly-or-derived class T.
	template <typename T>
	static int32 GTPC_CountEventGraphNodesOfClass(UBlueprint* BP)
	{
		UEdGraph* EventGraph = GTPC_FindEventGraph(BP);
		if (!IsValid(EventGraph)) return 0;
		int32 Count = 0;
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			if (Cast<T>(Node)) ++Count;
		}
		return Count;
	}

	// First event-graph node of class T, or null.
	template <typename T>
	static T* GTPC_FindEventGraphNodeOfClass(UBlueprint* BP)
	{
		UEdGraph* EventGraph = GTPC_FindEventGraph(BP);
		if (!IsValid(EventGraph)) return nullptr;
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			if (T* Typed = Cast<T>(Node)) return Typed;
		}
		return nullptr;
	}
}

// ============================================================================
// Test 1: add_node with schema-form scalar position_x/position_y lands the
// node at exactly those coordinates (not the (0,0)/cursor default).
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GraphToolParamContract, AddNode_ScalarPositionApplied, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonGraphToolParamContractTests_anon;

	GTPC_CleanupAsset(GTPC_Path_AddNodeScalarPos);
	UBlueprint* BP = GTPC_CreateActorBP(GTPC_Path_AddNodeScalarPos);
	UNTEST_ASSERT_PTR(BP);

	ClaireonBlueprintGraphTool_AddNode Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), GTPC_Path_AddNodeScalarPos);
	Args->SetStringField(TEXT("node_type"), TEXT("Branch"));
	Args->SetNumberField(TEXT("position_x"), 640.0);
	Args->SetNumberField(TEXT("position_y"), -256.0);

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);

	UK2Node_IfThenElse* Node = GTPC_FindBranchNode(BP);
	UNTEST_ASSERT_PTR(Node);
	UNTEST_EXPECT_EQ(Node->NodePosX, 640);
	UNTEST_EXPECT_EQ(Node->NodePosY, -256);

	GTPC_CleanupAsset(GTPC_Path_AddNodeScalarPos);
	co_return;
}

// ============================================================================
// Test 2: add_node with the position={x,y} object form lands the node at
// exactly those coordinates.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GraphToolParamContract, AddNode_ObjectPositionApplied, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonGraphToolParamContractTests_anon;

	GTPC_CleanupAsset(GTPC_Path_AddNodeObjectPos);
	UBlueprint* BP = GTPC_CreateActorBP(GTPC_Path_AddNodeObjectPos);
	UNTEST_ASSERT_PTR(BP);

	ClaireonBlueprintGraphTool_AddNode Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), GTPC_Path_AddNodeObjectPos);
	Args->SetStringField(TEXT("node_type"), TEXT("Branch"));
	Args->SetObjectField(TEXT("position"), GTPC_MakePositionObject(320.0, 480.0));

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);

	UK2Node_IfThenElse* Node = GTPC_FindBranchNode(BP);
	UNTEST_ASSERT_PTR(Node);
	UNTEST_EXPECT_EQ(Node->NodePosX, 320);
	UNTEST_EXPECT_EQ(Node->NodePosY, 480);

	GTPC_CleanupAsset(GTPC_Path_AddNodeObjectPos);
	co_return;
}

// ============================================================================
// Test 3: move_node accepts scalar-only, object-only, and both forms; the
// resulting position is exact each time, and the object wins when both forms
// are supplied.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GraphToolParamContract, MoveNode_ScalarObjectAndBothFormsExact, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonGraphToolParamContractTests_anon;

	GTPC_CleanupAsset(GTPC_Path_MoveNodeForms);
	UBlueprint* BP = GTPC_CreateActorBP(GTPC_Path_MoveNodeForms);
	UNTEST_ASSERT_PTR(BP);

	UK2Node_IfThenElse* Node = GTPC_AddBranchNodeDirect(BP);
	UNTEST_ASSERT_PTR(Node);
	const FString NodeGuidStr = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

	ClaireonBlueprintGraphTool_MoveNode Tool;

	// --- scalar-only form ---
	TSharedPtr<FJsonObject> ScalarArgs = MakeShared<FJsonObject>();
	ScalarArgs->SetStringField(TEXT("asset_path"), GTPC_Path_MoveNodeForms);
	ScalarArgs->SetStringField(TEXT("node_guid"), NodeGuidStr);
	ScalarArgs->SetNumberField(TEXT("position_x"), 100.0);
	ScalarArgs->SetNumberField(TEXT("position_y"), 200.0);
	IClaireonTool::FToolResult ScalarResult = Tool.Execute(ScalarArgs);
	UNTEST_ASSERT_FALSE(ScalarResult.bIsError);
	UNTEST_EXPECT_EQ(Node->NodePosX, 100);
	UNTEST_EXPECT_EQ(Node->NodePosY, 200);

	// --- object-only form ---
	TSharedPtr<FJsonObject> ObjectArgs = MakeShared<FJsonObject>();
	ObjectArgs->SetStringField(TEXT("asset_path"), GTPC_Path_MoveNodeForms);
	ObjectArgs->SetStringField(TEXT("node_guid"), NodeGuidStr);
	ObjectArgs->SetObjectField(TEXT("position"), GTPC_MakePositionObject(300.0, 400.0));
	IClaireonTool::FToolResult ObjectResult = Tool.Execute(ObjectArgs);
	UNTEST_ASSERT_FALSE(ObjectResult.bIsError);
	UNTEST_EXPECT_EQ(Node->NodePosX, 300);
	UNTEST_EXPECT_EQ(Node->NodePosY, 400);

	// --- both forms: the object wins ---
	TSharedPtr<FJsonObject> BothArgs = MakeShared<FJsonObject>();
	BothArgs->SetStringField(TEXT("asset_path"), GTPC_Path_MoveNodeForms);
	BothArgs->SetStringField(TEXT("node_guid"), NodeGuidStr);
	BothArgs->SetNumberField(TEXT("position_x"), 111.0);
	BothArgs->SetNumberField(TEXT("position_y"), 222.0);
	BothArgs->SetObjectField(TEXT("position"), GTPC_MakePositionObject(555.0, 666.0));
	IClaireonTool::FToolResult BothResult = Tool.Execute(BothArgs);
	UNTEST_ASSERT_FALSE(BothResult.bIsError);
	UNTEST_EXPECT_EQ(Node->NodePosX, 555);
	UNTEST_EXPECT_EQ(Node->NodePosY, 666);

	GTPC_CleanupAsset(GTPC_Path_MoveNodeForms);
	co_return;
}

// ============================================================================
// Test 4: a position object missing 'y' is a hard error (never a silent move
// to origin) and the node stays exactly where it was.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GraphToolParamContract, MoveNode_PartialObjectErrorsNodeUnmoved, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonGraphToolParamContractTests_anon;

	GTPC_CleanupAsset(GTPC_Path_MoveNodePartial);
	UBlueprint* BP = GTPC_CreateActorBP(GTPC_Path_MoveNodePartial);
	UNTEST_ASSERT_PTR(BP);

	UK2Node_IfThenElse* Node = GTPC_AddBranchNodeDirect(BP);
	UNTEST_ASSERT_PTR(Node);
	Node->NodePosX = 1234;
	Node->NodePosY = 5678;
	const FString NodeGuidStr = Node->NodeGuid.ToString(EGuidFormats::DigitsWithHyphens);

	// position={x} only -- no y.
	TSharedPtr<FJsonObject> PartialPos = MakeShared<FJsonObject>();
	PartialPos->SetNumberField(TEXT("x"), 999.0);

	ClaireonBlueprintGraphTool_MoveNode Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), GTPC_Path_MoveNodePartial);
	Args->SetStringField(TEXT("node_guid"), NodeGuidStr);
	Args->SetObjectField(TEXT("position"), PartialPos);

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(R.bIsError);
	UNTEST_EXPECT_STREQ(R.ErrorMessage,
		TEXT("position object is missing 'y'; pass both x and y (a partial position object is never zero-defaulted)"));

	// Node unmoved.
	UNTEST_EXPECT_EQ(Node->NodePosX, 1234);
	UNTEST_EXPECT_EQ(Node->NodePosY, 5678);

	GTPC_CleanupAsset(GTPC_Path_MoveNodePartial);
	co_return;
}

// ============================================================================
// Test 5: add_component parent_name attaches the new SCS node under the named
// existing child (verified by walking the SCS tree), and a bogus parent_name
// is a hard error that does not silently root the component.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GraphToolParamContract, AddComponent_ParentNameParentsAndBogusErrors, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonGraphToolParamContractTests_anon;

	GTPC_CleanupAsset(GTPC_Path_AddComponent);
	UBlueprint* BP = GTPC_CreateActorBP(GTPC_Path_AddComponent);
	UNTEST_ASSERT_PTR(BP);
	UNTEST_ASSERT_PTR(BP->SimpleConstructionScript.Get());

	ClaireonBlueprintGraphTool_AddComponent Tool;

	// Root-level child to parent under.
	TSharedPtr<FJsonObject> ChildAArgs = MakeShared<FJsonObject>();
	ChildAArgs->SetStringField(TEXT("asset_path"), GTPC_Path_AddComponent);
	ChildAArgs->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.SceneComponent"));
	ChildAArgs->SetStringField(TEXT("component_name"), TEXT("ParamContractChildA"));
	IClaireonTool::FToolResult ChildAResult = Tool.Execute(ChildAArgs);
	UNTEST_ASSERT_FALSE(ChildAResult.bIsError);

	// Attach under the existing child via parent_name (the schema-form field).
	TSharedPtr<FJsonObject> ChildBArgs = MakeShared<FJsonObject>();
	ChildBArgs->SetStringField(TEXT("asset_path"), GTPC_Path_AddComponent);
	ChildBArgs->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.SceneComponent"));
	ChildBArgs->SetStringField(TEXT("component_name"), TEXT("ParamContractChildB"));
	ChildBArgs->SetStringField(TEXT("parent_name"), TEXT("ParamContractChildA"));
	IClaireonTool::FToolResult ChildBResult = Tool.Execute(ChildBArgs);
	UNTEST_ASSERT_FALSE(ChildBResult.bIsError);

	// Walk the SCS tree: ChildB must hang under ChildA, not the root set.
	USCS_Node* ParentSCSNode = BP->SimpleConstructionScript->FindSCSNode(FName(TEXT("ParamContractChildA")));
	UNTEST_ASSERT_PTR(ParentSCSNode);
	bool bFoundUnderParent = false;
	for (USCS_Node* Child : ParentSCSNode->GetChildNodes())
	{
		if (IsValid(Child) && Child->GetVariableName() == FName(TEXT("ParamContractChildB")))
		{
			bFoundUnderParent = true;
		}
	}
	UNTEST_EXPECT_TRUE(bFoundUnderParent);

	// Bogus parent_name: hard error, and the component must not appear anywhere.
	TSharedPtr<FJsonObject> BogusArgs = MakeShared<FJsonObject>();
	BogusArgs->SetStringField(TEXT("asset_path"), GTPC_Path_AddComponent);
	BogusArgs->SetStringField(TEXT("component_class"), TEXT("/Script/Engine.SceneComponent"));
	BogusArgs->SetStringField(TEXT("component_name"), TEXT("ParamContractChildC"));
	BogusArgs->SetStringField(TEXT("parent_name"), TEXT("Bogus_DoesNotExist"));
	IClaireonTool::FToolResult BogusResult = Tool.Execute(BogusArgs);
	UNTEST_ASSERT_TRUE(BogusResult.bIsError);
	UNTEST_EXPECT_STREQ(BogusResult.ErrorMessage, TEXT("Parent component not found: Bogus_DoesNotExist"));
	UNTEST_EXPECT_TRUE(BP->SimpleConstructionScript->FindSCSNode(FName(TEXT("ParamContractChildC"))) == nullptr);

	GTPC_CleanupAsset(GTPC_Path_AddComponent);
	co_return;
}

// ============================================================================
// Test 6: add_node node_type=ComponentBoundEvent without component_name errors
// with a message that names the field, and the tool's GetInputSchema() output
// declares that same field (schema/impl agreement).
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GraphToolParamContract, AddNode_ComponentBoundEventMissingComponentName, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonGraphToolParamContractTests_anon;

	GTPC_CleanupAsset(GTPC_Path_CBEMissingField);
	UBlueprint* BP = GTPC_CreateActorBP(GTPC_Path_CBEMissingField);
	UNTEST_ASSERT_PTR(BP);

	ClaireonBlueprintGraphTool_AddNode Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), GTPC_Path_CBEMissingField);
	Args->SetStringField(TEXT("node_type"), TEXT("ComponentBoundEvent"));
	Args->SetStringField(TEXT("delegate_name"), TEXT("OnClicked"));
	// component_name deliberately omitted.

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(R.bIsError);
	UNTEST_EXPECT_STREQ(R.ErrorMessage,
		TEXT("Missing required field 'component_name' for ComponentBoundEvent node"));
	// Removed: UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("component_name"))).
	// The STREQ immediately above pins the entire message, and that literal
	// contains "component_name", so the Contains check was implied by an
	// assertion already made -- it could only fail in cases the STREQ had
	// already failed.

	// GetInputSchema() must declare the field the impl hard-requires.
	TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_PTR(Schema.Get());
	const TSharedPtr<FJsonObject>* SchemaProps = nullptr;
	UNTEST_ASSERT_TRUE(Schema->TryGetObjectField(TEXT("properties"), SchemaProps));
	UNTEST_EXPECT_TRUE((*SchemaProps)->HasField(TEXT("component_name")));

	GTPC_CleanupAsset(GTPC_Path_CBEMissingField);
	co_return;
}

// ============================================================================
// Test 7: add_node node_type=SpawnActor accepts the impl-required actor_class
// field, accepts target_class as its documented alias, and GetInputSchema()
// declares actor_class (schema/impl agreement).
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GraphToolParamContract, AddNode_SpawnActorActorClassAndTargetClassAlias, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonGraphToolParamContractTests_anon;

	GTPC_CleanupAsset(GTPC_Path_SpawnActorClass);
	UBlueprint* BP = GTPC_CreateActorBP(GTPC_Path_SpawnActorClass);
	UNTEST_ASSERT_PTR(BP);

	ClaireonBlueprintGraphTool_AddNode Tool;

	// --- schema-form actor_class ---
	TSharedPtr<FJsonObject> ActorClassArgs = MakeShared<FJsonObject>();
	ActorClassArgs->SetStringField(TEXT("asset_path"), GTPC_Path_SpawnActorClass);
	ActorClassArgs->SetStringField(TEXT("node_type"), TEXT("SpawnActor"));
	ActorClassArgs->SetStringField(TEXT("actor_class"), TEXT("/Script/Engine.StaticMeshActor"));
	IClaireonTool::FToolResult ActorClassResult = Tool.Execute(ActorClassArgs);
	UNTEST_ASSERT_FALSE(ActorClassResult.bIsError);
	UNTEST_EXPECT_EQ(GTPC_CountEventGraphNodesOfClass<UK2Node_SpawnActorFromClass>(BP), 1);

	// --- legacy alias target_class (the shape the old schema described) ---
	TSharedPtr<FJsonObject> AliasArgs = MakeShared<FJsonObject>();
	AliasArgs->SetStringField(TEXT("asset_path"), GTPC_Path_SpawnActorClass);
	AliasArgs->SetStringField(TEXT("node_type"), TEXT("SpawnActor"));
	AliasArgs->SetStringField(TEXT("target_class"), TEXT("/Script/Engine.StaticMeshActor"));
	IClaireonTool::FToolResult AliasResult = Tool.Execute(AliasArgs);
	UNTEST_ASSERT_FALSE(AliasResult.bIsError);
	UNTEST_EXPECT_EQ(GTPC_CountEventGraphNodesOfClass<UK2Node_SpawnActorFromClass>(BP), 2);

	// GetInputSchema() must declare the field the impl hard-requires.
	TSharedPtr<FJsonObject> Schema = Tool.GetInputSchema();
	UNTEST_ASSERT_PTR(Schema.Get());
	const TSharedPtr<FJsonObject>* SchemaProps = nullptr;
	UNTEST_ASSERT_TRUE(Schema->TryGetObjectField(TEXT("properties"), SchemaProps));
	UNTEST_EXPECT_TRUE((*SchemaProps)->HasField(TEXT("actor_class")));

	GTPC_CleanupAsset(GTPC_Path_SpawnActorClass);
	co_return;
}

// ============================================================================
// Test 8: add_node node_type=AssignDelegate creates the companion CustomEvent
// AND actually wires its delegate output to the Assign node's delegate input
// (the success claim is now gated on TryCreateConnection; an unwired companion
// must surface as a warning, so a clean run has the link and no such warning).
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, GraphToolParamContract, AddNode_AssignDelegateCompanionEventWired, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonGraphToolParamContractTests_anon;

	GTPC_CleanupAsset(GTPC_Path_AssignDelegate);
	UBlueprint* BP = GTPC_CreateActorBP(GTPC_Path_AssignDelegate);
	UNTEST_ASSERT_PTR(BP);

	ClaireonBlueprintGraphTool_AddNode Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), GTPC_Path_AssignDelegate);
	Args->SetStringField(TEXT("node_type"), TEXT("AssignDelegate"));
	Args->SetStringField(TEXT("delegate_name"), TEXT("OnActorBeginOverlap"));
	Args->SetStringField(TEXT("event_name"), TEXT("ParamContract_OverlapEvent"));

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);

	// No "NOT wired" warning on the happy path.
	bool bHasNotWiredWarning = false;
	for (const FString& Warning : R.Warnings)
	{
		if (Warning.Contains(TEXT("NOT wired")))
		{
			bHasNotWiredWarning = true;
		}
	}
	UNTEST_EXPECT_FALSE(bHasNotWiredWarning);

	UK2Node_AssignDelegate* AssignNode = GTPC_FindEventGraphNodeOfClass<UK2Node_AssignDelegate>(BP);
	UNTEST_ASSERT_PTR(AssignNode);
	UK2Node_CustomEvent* EventNode = GTPC_FindEventGraphNodeOfClass<UK2Node_CustomEvent>(BP);
	UNTEST_ASSERT_PTR(EventNode);

	// The Assign node's delegate input must be linked to the companion event's
	// delegate output pin.
	UEdGraphPin* DelegatePin = AssignNode->GetDelegatePin();
	UNTEST_ASSERT_PTR(DelegatePin);
	UEdGraphPin* EventDelegatePin = EventNode->FindPin(UK2Node_Event::DelegateOutputName);
	UNTEST_ASSERT_PTR(EventDelegatePin);
	UNTEST_EXPECT_TRUE(DelegatePin->LinkedTo.Contains(EventDelegatePin));

	GTPC_CleanupAsset(GTPC_Path_AssignDelegate);
	co_return;
}

#endif // WITH_UNTESTED
