// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Baseline pinning tests for Workstream A (node creation), Stage 001 of the
// Claireon BP feedback plan (Work #6704). These pin behaviors that are
// ALREADY correct today so later fix stages (010/011) run against a suite
// that locks the pre-fix baseline:
//   - B1 core: the designer report's "plausible working call" (Generic +
//     class_name='K2Node_LatentGameplayTaskCall' + proxy node_properties)
//     already creates a node with then/delegate pins.
//   - bp_create blueprint_type='MacroLibrary' currently yields BPTYPE_Normal
//     (blueprint_type is not read yet; flips in Stage 011).

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonBlueprintGraphTool_AddMacro.h"
#include "Tools/ClaireonBlueprintGraphTool_AddNode.h"
#include "Tools/ClaireonBlueprintGraphTool_Create.h"
#include "Tools/ClaireonBlueprintGraphTool_ListNodeTypes.h"
#include "Tools/ClaireonBlueprintGraphEditToolBase_Internal.h"
#include "ClaireonBlueprintHelpers.h"
#include "ClaireonBlueprintNodeTypeRegistry.h"
#include "Tools/IClaireonTool.h"
#include "ClaireonSessionManager.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_Tunnel.h"
#include "UObject/Interface.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonNCTTestsInternal
{
	// Release any session auto-opened on the asset, then force-delete it.
	// Safe to call even if no session was ever opened and even if the asset
	// does not exist.
	static void NCT_CleanupAsset(const FString& AssetPath)
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

	static UBlueprint* NCT_CreateActorBP(const FString& AssetPath)
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

	static UEdGraph* NCT_FindEventGraph(UBlueprint* BP)
	{
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (IsValid(G)) return G;
		}
		return nullptr;
	}

	// First event-graph node whose GetClass()->GetName() equals ClassName
	// (string compare, not Cast<T>, so callers can probe classes this module
	// has no compile-time header for -- see ws-a-node-creation.md's note on
	// K2Node_LatentGameplayTaskCall/K2Node_LatentAbilityCall requiring no new
	// Build.cs dependency).
	static UEdGraphNode* NCT_FindEventGraphNodeByClassName(UBlueprint* BP, const FString& ClassName)
	{
		UEdGraph* EventGraph = NCT_FindEventGraph(BP);
		if (!IsValid(EventGraph)) return nullptr;
		for (UEdGraphNode* Node : EventGraph->Nodes)
		{
			if (IsValid(Node) && Node->GetClass()->GetName() == ClassName)
			{
				return Node;
			}
		}
		return nullptr;
	}

	static bool NCT_NodeHasPin(UEdGraphNode* Node, const TCHAR* PinName)
	{
		if (!IsValid(Node)) return false;
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin && Pin->PinName.ToString().Equals(PinName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}
}

// ============================================================================
// B1 core (resolved, pinning only): Generic + class_name=
// 'K2Node_LatentGameplayTaskCall' + a full proxy node_properties bag
// (mirroring the async-task ProxyFactoryClass/ProxyClass/
// ProxyFactoryFunctionName reflection fields UK2Node_BaseAsyncTask needs)
// already produces a node with the exec 'then' pin and the proxy class's
// delegate output pin ('OnComplete', from the ClaireonTestAsyncAction
// fixture). Pinned BEFORE the WS-A alias work (Stage 010) lands so that work
// cannot regress this already-working path.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, GenericLatentGameplayTaskCall_HasThenAndDelegatePins, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNCTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_GenericLatentTaskCall");

	NCT_CleanupAsset(AssetPath);
	UBlueprint* BP = NCT_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("ProxyFactoryClass"), TEXT("ClaireonTestAsyncAction"));
	Props->SetStringField(TEXT("ProxyClass"), TEXT("ClaireonTestAsyncAction"));
	Props->SetStringField(TEXT("ProxyFactoryFunctionName"), TEXT("ClaireonTestAsyncDelay"));

	ClaireonBlueprintGraphTool_AddNode Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("node_type"), TEXT("Generic"));
	Args->SetStringField(TEXT("class_name"), TEXT("K2Node_LatentGameplayTaskCall"));
	Args->SetObjectField(TEXT("node_properties"), Props);

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);

	UEdGraphNode* Created = NCT_FindEventGraphNodeByClassName(BP, TEXT("K2Node_LatentGameplayTaskCall"));
	UNTEST_ASSERT_PTR(Created);

	UNTEST_EXPECT_TRUE(NCT_NodeHasPin(Created, TEXT("then")));
	UNTEST_EXPECT_TRUE(NCT_NodeHasPin(Created, TEXT("OnComplete")));

	NCT_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// bp_create blueprint_type='MacroLibrary' pinning test (pre-fix): the field
// is advertised in the schema but never read by Execute today, so the new
// Blueprint is created as BPTYPE_Normal regardless of the requested type.
// This flips in Stage 011 (A2); this test is updated there, not left stale.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, Create_MacroLibraryBlueprintType_YieldsMacroLibrary, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNCTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_CreateMacroLibrary");

	NCT_CleanupAsset(AssetPath);

	// blueprint_type used to be advertised but never read, so this came out Normal.
	// A MacroLibrary also has no EventGraph, which used to turn a successful create
	// into "Failed to find EventGraph in newly created Blueprint".
	ClaireonBlueprintGraphTool_Create Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("blueprint_type"), TEXT("MacroLibrary"));

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[NodeCreation] MacroLibrary create failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);
	UNTEST_EXPECT_TRUE(BP->BlueprintType == BPTYPE_MacroLibrary);
	// parent_class omitted -> AActor, matching UBlueprintMacroFactory.
	UNTEST_EXPECT_TRUE(BP->ParentClass == AActor::StaticClass());
	// No ubergraph, and the graph-less response still rendered.
	UNTEST_EXPECT_EQ(BP->UbergraphPages.Num(), 0);

	NCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, Create_InterfaceBlueprintType_YieldsInterface, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNCTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_CreateInterface");

	NCT_CleanupAsset(AssetPath);

	ClaireonBlueprintGraphTool_Create Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("blueprint_type"), TEXT("Interface"));

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[NodeCreation] Interface create failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);
	UNTEST_EXPECT_TRUE(BP->BlueprintType == BPTYPE_Interface);
	// parent_class omitted -> UInterface, matching UBlueprintInterfaceFactory.
	UNTEST_EXPECT_TRUE(BP->ParentClass == UInterface::StaticClass());

	NCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, Create_LevelScriptBlueprintType_RejectedByName, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNCTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_CreateLevelScript");

	NCT_CleanupAsset(AssetPath);

	// LevelScript was advertised and unimplementable through this flow. It must be
	// rejected by name, not silently downgraded to Normal.
	ClaireonBlueprintGraphTool_Create Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("blueprint_type"), TEXT("LevelScript"));

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("LevelScript")));

	NCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, Create_NoBlueprintType_StaysNormal, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNCTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_CreateDefaultType");

	NCT_CleanupAsset(AssetPath);

	ClaireonBlueprintGraphTool_Create Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("parent_class"), TEXT("Actor"));

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);
	UNTEST_EXPECT_TRUE(BP->BlueprintType == BPTYPE_Normal);
	UNTEST_EXPECT_TRUE(BP->UbergraphPages.Num() > 0);

	NCT_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// A2 (Stage 011): bp_add_macro creates a macro graph with its tunnel pair and
// the requested pins, and works on a MacroLibrary that has no graphs at all.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, AddMacro_CreatesGraphWithTunnelsAndPins, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNCTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_AddMacro");

	NCT_CleanupAsset(AssetPath);
	UBlueprint* BP = NCT_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);
	const int32 MacroCountBefore = BP->MacroGraphs.Num();

	TSharedPtr<FJsonObject> InputEntry = MakeShared<FJsonObject>();
	InputEntry->SetStringField(TEXT("name"), TEXT("Amount"));
	InputEntry->SetStringField(TEXT("type"), TEXT("float"));
	TSharedPtr<FJsonObject> OutputEntry = MakeShared<FJsonObject>();
	OutputEntry->SetStringField(TEXT("name"), TEXT("Applied"));
	OutputEntry->SetStringField(TEXT("type"), TEXT("bool"));

	TArray<TSharedPtr<FJsonValue>> Inputs;
	Inputs.Add(MakeShared<FJsonValueObject>(InputEntry));
	TArray<TSharedPtr<FJsonValue>> Outputs;
	Outputs.Add(MakeShared<FJsonValueObject>(OutputEntry));

	ClaireonBlueprintGraphTool_AddMacro Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("macro_name"), TEXT("ApplyDamage"));
	Args->SetArrayField(TEXT("inputs"), Inputs);
	Args->SetArrayField(TEXT("outputs"), Outputs);

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[NodeCreation] add_macro failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	FString ReportedGraphName;
	UNTEST_ASSERT_TRUE(R.Data->TryGetStringField(TEXT("graph_name"), ReportedGraphName));
	UNTEST_EXPECT_TRUE(ReportedGraphName == TEXT("ApplyDamage"));

	UNTEST_EXPECT_EQ(BP->MacroGraphs.Num(), MacroCountBefore + 1);

	UEdGraph* MacroGraph = ClaireonBlueprintHelpers::FindGraphByName(BP, TEXT("ApplyDamage"));
	UNTEST_ASSERT_PTR(MacroGraph);

	// Entry tunnel carries inputs[] as OUTPUT pins; exit tunnel carries outputs[]
	// as INPUT pins.
	UEdGraphNode* EntryTunnel = nullptr;
	UEdGraphNode* ExitTunnel = nullptr;
	for (UEdGraphNode* Node : MacroGraph->Nodes)
	{
		if (UK2Node_Tunnel* Tunnel = Cast<UK2Node_Tunnel>(Node); IsValid(Tunnel))
		{
			if (Tunnel->bCanHaveOutputs && !IsValid(EntryTunnel)) { EntryTunnel = Tunnel; }
			else if (Tunnel->bCanHaveInputs && !IsValid(ExitTunnel)) { ExitTunnel = Tunnel; }
		}
	}
	UNTEST_ASSERT_PTR(EntryTunnel);
	UNTEST_ASSERT_PTR(ExitTunnel);
	UNTEST_EXPECT_TRUE(NCT_NodeHasPin(EntryTunnel, TEXT("Amount")));
	UNTEST_EXPECT_TRUE(NCT_NodeHasPin(ExitTunnel, TEXT("Applied")));

	NCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, AddMacro_DuplicateNameIsNamedError, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNCTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_AddMacroDup");

	NCT_CleanupAsset(AssetPath);
	UBlueprint* BP = NCT_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	auto AddMacroNamed = [](const TCHAR* Path, const TCHAR* Name) -> IClaireonTool::FToolResult
	{
		ClaireonBlueprintGraphTool_AddMacro Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), Path);
		Args->SetStringField(TEXT("macro_name"), Name);
		return Tool.Execute(Args);
	};

	IClaireonTool::FToolResult First = AddMacroNamed(AssetPath, TEXT("OnlyOnce"));
	UNTEST_ASSERT_FALSE(First.bIsError);

	// A collision must be reported, not silently renamed to OnlyOnce_1.
	IClaireonTool::FToolResult Second = AddMacroNamed(AssetPath, TEXT("OnlyOnce"));
	UNTEST_EXPECT_TRUE(Second.bIsError);
	UNTEST_EXPECT_TRUE(Second.ErrorMessage.Contains(TEXT("already exists")));

	NCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, AddMacro_BadPinTypeAbortsBeforeMutating, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNCTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_AddMacroBadType");

	NCT_CleanupAsset(AssetPath);
	UBlueprint* BP = NCT_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);
	const int32 MacroCountBefore = BP->MacroGraphs.Num();

	TSharedPtr<FJsonObject> BadInput = MakeShared<FJsonObject>();
	BadInput->SetStringField(TEXT("name"), TEXT("Broken"));
	BadInput->SetStringField(TEXT("type"), TEXT("NotAType_XYZ"));
	TArray<TSharedPtr<FJsonValue>> Inputs;
	Inputs.Add(MakeShared<FJsonValueObject>(BadInput));

	ClaireonBlueprintGraphTool_AddMacro Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("macro_name"), TEXT("ShouldNotExist"));
	Args->SetArrayField(TEXT("inputs"), Inputs);

	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_EXPECT_TRUE(R.bIsError);
	// No half-built macro left behind.
	UNTEST_EXPECT_EQ(BP->MacroGraphs.Num(), MacroCountBefore);
	UNTEST_EXPECT_NULLPTR(ClaireonBlueprintHelpers::FindGraphByName(BP, TEXT("ShouldNotExist")));

	NCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, AddMacro_OnGraphlessMacroLibraryAutoOpens, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNCTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_MacroLibRoundTrip");

	NCT_CleanupAsset(AssetPath);

	// A MacroLibrary has no EventGraph and, freshly created, no graphs at all. The
	// asset_path auto-open used to hard-error on the default graph_name, which made
	// bp_add_macro unable to populate the very asset it exists for.
	{
		ClaireonBlueprintGraphTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("blueprint_type"), TEXT("MacroLibrary"));
		IClaireonTool::FToolResult R = CreateTool.Execute(Args);
		if (R.bIsError)
		{
			UE_LOG(LogTemp, Error, TEXT("[NodeCreation] MacroLibrary create failed: %s"), *R.ErrorMessage);
		}
		UNTEST_ASSERT_FALSE(R.bIsError);
	}
	FClaireonSessionManager::Get().ReleaseByAssetPath(AssetPath);

	ClaireonBlueprintGraphTool_AddMacro Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), AssetPath);
	Args->SetStringField(TEXT("macro_name"), TEXT("LibMacro"));
	IClaireonTool::FToolResult R = Tool.Execute(Args);
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[NodeCreation] add_macro on MacroLibrary failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	FString ReportedGraphName;
	UNTEST_ASSERT_TRUE(R.Data.IsValid());
	UNTEST_ASSERT_TRUE(R.Data->TryGetStringField(TEXT("graph_name"), ReportedGraphName));
	UNTEST_EXPECT_TRUE(ReportedGraphName == TEXT("LibMacro"));

	UBlueprint* BP = Cast<UBlueprint>(FSoftObjectPath(
		FString(AssetPath) + TEXT(".") + FPackageName::GetShortName(AssetPath)).TryLoad());
	UNTEST_ASSERT_PTR(BP);
	UNTEST_EXPECT_PTR(ClaireonBlueprintHelpers::FindGraphByName(BP, TEXT("LibMacro")));

	NCT_CleanupAsset(AssetPath);
	co_return;
}

// ============================================================================
// A1 (Stage 010): the LatentAbilityCall / LatentGameplayTaskCall spellings
// resolve, CallFunction on a UGameplayTask factory promotes to the dedicated
// latent node instead of a plain CallFunction, and the AsyncAction rejection
// names the route that works.
//
// Node classes are compared by GetClass()->GetName() string, never Cast<T>:
// UK2Node_LatentAbilityCall / UK2Node_LatentGameplayTaskCall live in editor
// modules Claireon deliberately does not depend on.
// ============================================================================
namespace ClaireonA1TestsInternal
{
	static IClaireonTool::FToolResult A1_AddNode(
		const TCHAR* AssetPath, TFunctionRef<void(TSharedPtr<FJsonObject>&)> Configure)
	{
		ClaireonBlueprintGraphTool_AddNode Tool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Configure(Args);
		return Tool.Execute(Args);
	}
} // namespace ClaireonA1TestsInternal

UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, AliasResolve_LatentGameplayTaskCallSpelling, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNCTTestsInternal;
	using namespace ClaireonA1TestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_AliasLatentTask");

	NCT_CleanupAsset(AssetPath);
	UBlueprint* BP = NCT_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	// The bare spelling used to hit bp_add_node's "Unsupported node type" else.
	IClaireonTool::FToolResult R = A1_AddNode(AssetPath, [](TSharedPtr<FJsonObject>& Args)
	{
		Args->SetStringField(TEXT("node_type"), TEXT("LatentGameplayTaskCall"));
		Args->SetStringField(TEXT("function_name"), TEXT("ClaireonTestWaitForThing"));
		Args->SetStringField(TEXT("function_class"), TEXT("ClaireonTestGameplayTask"));
	});
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[NodeCreation] alias add_node failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UEdGraphNode* Created = NCT_FindEventGraphNodeByClassName(BP, TEXT("K2Node_LatentGameplayTaskCall"));
	UNTEST_EXPECT_PTR(Created);

	NCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, AliasResolve_LatentAbilityCallSpellingPicksByFactoryType, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNCTTestsInternal;
	using namespace ClaireonA1TestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_AliasLatentAbility");

	NCT_CleanupAsset(AssetPath);
	UBlueprint* BP = NCT_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	// LatentAbilityCall is a hint, but the resolved factory decides: this fixture is
	// a plain UGameplayTask, so the correct node class is the GameplayTask one.
	IClaireonTool::FToolResult R = A1_AddNode(AssetPath, [](TSharedPtr<FJsonObject>& Args)
	{
		Args->SetStringField(TEXT("node_type"), TEXT("LatentAbilityCall"));
		Args->SetStringField(TEXT("function_name"), TEXT("ClaireonTestWaitForThing"));
		Args->SetStringField(TEXT("function_class"), TEXT("ClaireonTestGameplayTask"));
	});
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[NodeCreation] ability alias add_node failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UEdGraphNode* AsTaskCall = NCT_FindEventGraphNodeByClassName(BP, TEXT("K2Node_LatentGameplayTaskCall"));
	UEdGraphNode* AsAbilityCall = NCT_FindEventGraphNodeByClassName(BP, TEXT("K2Node_LatentAbilityCall"));
	UNTEST_EXPECT_TRUE(AsTaskCall != nullptr || AsAbilityCall != nullptr);
	// Factory type wins over the spelling.
	UNTEST_EXPECT_PTR(AsTaskCall);

	NCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, CallFunction_GameplayTaskFactoryPromotesToLatentNode, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNCTTestsInternal;
	using namespace ClaireonA1TestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_CallFnPromotion");

	NCT_CleanupAsset(AssetPath);
	UBlueprint* BP = NCT_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	// node_type='CallFunction' on a latent factory used to emit a plain
	// K2Node_CallFunction, which is the wrong node class for a task.
	IClaireonTool::FToolResult R = A1_AddNode(AssetPath, [](TSharedPtr<FJsonObject>& Args)
	{
		Args->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		Args->SetStringField(TEXT("function_name"), TEXT("ClaireonTestWaitForThing"));
		// CallFunction resolves its owner from function_class; class_name is the
		// Generic route's parameter and would leave the function unresolved.
		Args->SetStringField(TEXT("function_class"), TEXT("ClaireonTestGameplayTask"));
	});
	if (R.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[NodeCreation] CallFunction promotion failed: %s"), *R.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(R.bIsError);

	UEdGraphNode* Promoted = NCT_FindEventGraphNodeByClassName(BP, TEXT("K2Node_LatentGameplayTaskCall"));
	UEdGraphNode* PlainCall = NCT_FindEventGraphNodeByClassName(BP, TEXT("K2Node_CallFunction"));
	UNTEST_EXPECT_PTR(Promoted);
	// The whole point: it must NOT have stayed a plain CallFunction.
	UNTEST_EXPECT_NULLPTR(PlainCall);

	NCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, AsyncAction_GameplayTaskFactoryErrorNamesWorkingRoute, UNTEST_TIMEOUTMS(60000))
{
	using namespace ClaireonNCTTestsInternal;
	using namespace ClaireonA1TestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_AsyncActionHint");

	NCT_CleanupAsset(AssetPath);
	UBlueprint* BP = NCT_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	// AsyncAction genuinely cannot host a GameplayTask factory. The error has to
	// point at the route that can, instead of only reporting the type mismatch.
	IClaireonTool::FToolResult R = A1_AddNode(AssetPath, [](TSharedPtr<FJsonObject>& Args)
	{
		Args->SetStringField(TEXT("node_type"), TEXT("AsyncAction"));
		Args->SetStringField(TEXT("function_name"), TEXT("ClaireonTestWaitForThing"));
		Args->SetStringField(TEXT("function_class"), TEXT("ClaireonTestGameplayTask"));
	});

	UNTEST_EXPECT_TRUE(R.bIsError);
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("UGameplayTask")));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("LatentGameplayTaskCall")));

	NCT_CleanupAsset(AssetPath);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, ResolveNodeTypeAlias_RewritesLatentSpellingsToGeneric, UNTEST_TIMEOUTMS(30000))
{
	// Direct unit coverage of the alias layer, independent of any asset.
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_type"), TEXT("LatentGameplayTaskCall"));
		ClaireonNodeTypeAlias::ResolveNodeTypeAlias(Params);

		FString NodeType, ClassName;
		UNTEST_ASSERT_TRUE(Params->TryGetStringField(TEXT("node_type"), NodeType));
		UNTEST_ASSERT_TRUE(Params->TryGetStringField(TEXT("class_name"), ClassName));
		UNTEST_EXPECT_TRUE(NodeType == TEXT("Generic"));
		UNTEST_EXPECT_TRUE(ClassName == TEXT("K2Node_LatentGameplayTaskCall"));
	}
	{
		// No factory named -> the spelling alone decides.
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_type"), TEXT("LatentAbilityCall"));
		ClaireonNodeTypeAlias::ResolveNodeTypeAlias(Params);

		FString NodeType, ClassName;
		UNTEST_ASSERT_TRUE(Params->TryGetStringField(TEXT("node_type"), NodeType));
		UNTEST_ASSERT_TRUE(Params->TryGetStringField(TEXT("class_name"), ClassName));
		UNTEST_EXPECT_TRUE(NodeType == TEXT("Generic"));
		UNTEST_EXPECT_TRUE(ClassName == TEXT("K2Node_LatentAbilityCall"));
	}
	{
		// A plain alias must not be misrouted by the new branch.
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("node_type"), TEXT("Branch"));
		ClaireonNodeTypeAlias::ResolveNodeTypeAlias(Params);

		FString NodeType;
		UNTEST_ASSERT_TRUE(Params->TryGetStringField(TEXT("node_type"), NodeType));
		UNTEST_EXPECT_TRUE(NodeType == TEXT("Branch"));
		UNTEST_EXPECT_FALSE(Params->HasField(TEXT("class_name")));
	}

	co_return;
}

// ============================================================================
// A3 (Stage 012): bp_list_node_types returns the shared registry as structured
// data, and the registry is the same table bp_add_node dispatches on.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, ListNodeTypes_ReturnsStructuredArrayCoveringRegistry, UNTEST_TIMEOUTMS(30000))
{
	ClaireonBlueprintGraphTool_ListNodeTypes Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	// A real array, readable without substring parsing -- the sibling
	// *_list_node_types tools return a text blob and this one deliberately does not.
	const TArray<TSharedPtr<FJsonValue>>* NodeTypes = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetArrayField(TEXT("node_types"), NodeTypes));
	UNTEST_ASSERT_TRUE(NodeTypes != nullptr);

	// One entry per registry row, and every entry carries the full field set.
	const TArray<ClaireonBlueprintNodeTypes::FNodeTypeInfo>& Registry =
		ClaireonBlueprintNodeTypes::GetRegistry();
	UNTEST_ASSERT_TRUE(Registry.Num() > 0);
	UNTEST_EXPECT_EQ(NodeTypes->Num(), Registry.Num());

	TSet<FString> ReportedAliases;
	for (const TSharedPtr<FJsonValue>& Value : *NodeTypes)
	{
		TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
		UNTEST_ASSERT_TRUE(Entry.IsValid());

		FString Alias, Kind, Description;
		UNTEST_ASSERT_TRUE(Entry->TryGetStringField(TEXT("alias"), Alias));
		UNTEST_ASSERT_TRUE(Entry->TryGetStringField(TEXT("kind"), Kind));
		UNTEST_ASSERT_TRUE(Entry->TryGetStringField(TEXT("description"), Description));
		const TArray<TSharedPtr<FJsonValue>>* RequiredParams = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* OptionalParams = nullptr;
		UNTEST_ASSERT_TRUE(Entry->TryGetArrayField(TEXT("required_params"), RequiredParams));
		UNTEST_ASSERT_TRUE(Entry->TryGetArrayField(TEXT("optional_params"), OptionalParams));

		UNTEST_EXPECT_FALSE(Alias.IsEmpty());
		UNTEST_EXPECT_FALSE(Description.IsEmpty());
		UNTEST_EXPECT_TRUE(Kind == TEXT("factory") || Kind == TEXT("inline")
			|| Kind == TEXT("shorthand") || Kind == TEXT("generic"));

		ReportedAliases.Add(Alias);
	}

	for (const ClaireonBlueprintNodeTypes::FNodeTypeInfo& Info : Registry)
	{
		UNTEST_EXPECT_TRUE(ReportedAliases.Contains(FString(Info.Alias)));
	}

	// The escape hatch is disclosed, not left to prose elsewhere.
	FString EscapeHatch;
	UNTEST_ASSERT_TRUE(R.Data->TryGetStringField(TEXT("generic_escape_hatch"), EscapeHatch));
	UNTEST_EXPECT_TRUE(EscapeHatch.Contains(TEXT("Generic")));
	UNTEST_EXPECT_TRUE(EscapeHatch.Contains(TEXT("class_name")));

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, ListNodeTypes_FilterNarrowsByAlias, UNTEST_TIMEOUTMS(30000))
{
	ClaireonBlueprintGraphTool_ListNodeTypes Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("filter"), TEXT("Switch"));
	IClaireonTool::FToolResult R = Tool.Execute(Args);
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* NodeTypes = nullptr;
	UNTEST_ASSERT_TRUE(R.Data->TryGetArrayField(TEXT("node_types"), NodeTypes));
	UNTEST_ASSERT_TRUE(NodeTypes != nullptr);
	UNTEST_EXPECT_TRUE(NodeTypes->Num() > 0);

	for (const TSharedPtr<FJsonValue>& Value : *NodeTypes)
	{
		TSharedPtr<FJsonObject> Entry = Value.IsValid() ? Value->AsObject() : nullptr;
		UNTEST_ASSERT_TRUE(Entry.IsValid());
		FString Alias;
		UNTEST_ASSERT_TRUE(Entry->TryGetStringField(TEXT("alias"), Alias));
		UNTEST_EXPECT_TRUE(Alias.Contains(TEXT("Switch"), ESearchCase::IgnoreCase));
	}

	// total_count still reports the unfiltered size.
	int32 Returned = 0;
	int32 Total = 0;
	UNTEST_ASSERT_TRUE(R.Data->TryGetNumberField(TEXT("returned_count"), Returned));
	UNTEST_ASSERT_TRUE(R.Data->TryGetNumberField(TEXT("total_count"), Total));
	UNTEST_EXPECT_EQ(Returned, NodeTypes->Num());
	UNTEST_EXPECT_TRUE(Total > Returned);

	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BPFeedbackNodeCreation, ListNodeTypes_RequiredParamClaimsAreEnforced, UNTEST_TIMEOUTMS(120000))
{
	using namespace ClaireonNCTTestsInternal;

	static const TCHAR* AssetPath = TEXT("/Game/__MCPTests/BP_NCT_RequiredParams");

	NCT_CleanupAsset(AssetPath);
	UBlueprint* BP = NCT_CreateActorBP(AssetPath);
	UNTEST_ASSERT_PTR(BP);

	// For every registry entry that claims a required param, omitting it must fail.
	// Skipped deliberately:
	//  - Generic: its required class_name IS the escape hatch, covered elsewhere.
	//  - Tunnel / FunctionEntry: find-only, they report "not found" rather than a
	//    missing-param error, which is a different contract.
	const TSet<FString> Skip = {TEXT("Generic"), TEXT("Tunnel"), TEXT("FunctionEntry")};

	int32 Checked = 0;
	for (const ClaireonBlueprintNodeTypes::FNodeTypeInfo& Info : ClaireonBlueprintNodeTypes::GetRegistry())
	{
		if (Info.RequiredParams.Num() == 0) { continue; }
		const FString Alias = Info.Alias;
		if (Skip.Contains(Alias)) { continue; }

		ClaireonBlueprintGraphTool_AddNode AddNodeTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("node_type"), Alias);
		// Every required param omitted on purpose.
		IClaireonTool::FToolResult R = AddNodeTool.Execute(Args);

		if (!R.bIsError)
		{
			UE_LOG(LogTemp, Error,
				TEXT("[NodeCreation] node_type '%s' claims required params (%s) but succeeded without them"),
				*Alias, *FString::Join(Info.RequiredParams, TEXT(", ")));
		}
		UNTEST_EXPECT_TRUE(R.bIsError);
		++Checked;
	}
	UNTEST_EXPECT_TRUE(Checked > 0);

	NCT_CleanupAsset(AssetPath);
	co_return;
}

#endif // WITH_UNTESTED
