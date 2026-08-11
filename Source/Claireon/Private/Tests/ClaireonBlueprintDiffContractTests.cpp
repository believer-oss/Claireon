// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// WI-5 regression tests: bp_diff parameter contracts and false "no differences"
// verdicts.
//
//  - Whole-graph adds/removes must count the contained nodes so the counters
//    agree with the equivalent in-place edits (previously a one-sided graph
//    counted as a single NodesAdded++/NodesRemoved++).
//  - Unknown or empty 'sections' selections must error instead of silently
//    diffing nothing and reporting a 0/0/0/0 success.
//  - sections=['cdo'] must produce a real CDO diff verdict (cdo.fields_changed
//    plus the 'differs' bool), never a bare "0 nodes" no-diff claim.
//  - 'property_filter' is honored by the cdo/scs sections and errors when it
//    could not apply to any selected section.
//  - Identical assets report all counters zero and differs=false.
//
// Creates throwaway Blueprints under /Game/__MCPTests/, drives the tool through
// its JSON Execute entry, and asserts on the structured Data payload and exact
// error text.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonTool_BlueprintDiff.h"
#include "Tools/IClaireonTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraphNode_Comment.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Pawn.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonBlueprintDiffContractTestsInternal
{

// File-local discriminator prefix: BPDiffCT_ (anon namespaces are not isolation
// under unity batching).

// True only when the fixture actually has a .uasset on disk.
//
// BPDiffCT_CreateBP builds its Blueprint in a package created with
// CreatePackage() and never saves it, and bp_diff does not save either -- so the
// fixture normally lives in memory only. Deleting an in-memory fixture buys
// nothing, and every ObjectTools::ForceDeleteObjects call runs a whole-object-graph
// referencer scan, which is the trigger for the nondeterministic
// Niagara-serialization crash documented in
// Docs/llm/todo/claireon-untest-harness-reliability.md item 1. (This suite's
// SectionsCdo_ReportsCdoDelta_NotSilentNoDiff is one of the tests that crash has
// landed on.)
//
// The check is kept rather than dropping the delete outright because
// /Game/__MCPTests is deliberately NOT gitignored: a stale .uasset left by an
// older build or a crashed run must still be cleaned so `git status --porcelain
// -- Content/` stays empty.
bool BPDiffCT_HasFileOnDisk(const FString& AssetOrPackagePath)
{
	const FString PackageName = FPackageName::ObjectPathToPackageName(AssetOrPackagePath);
	FString FileName;
	if (!FPackageName::TryConvertLongPackageNameToFilename(
			PackageName, FileName, FPackageName::GetAssetPackageExtension()))
	{
		return false;
	}
	return FPaths::FileExists(FileName);
}

void BPDiffCT_Cleanup(const FString& PackagePath)
{
	// In-memory fixture: nothing on disk, nothing to clean, no referencer scan.
	if (!BPDiffCT_HasFileOnDisk(PackagePath))
	{
		return;
	}

	const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetShortName(PackagePath);
	if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
	{
		TArray<UObject*> AssetsToDelete;
		AssetsToDelete.Add(Asset);
		ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
	}
}

// Create (or reuse) a compiled Blueprint fixture at PackagePath with the given
// parent class. Returns nullptr on failure. Caller deletes via BPDiffCT_Cleanup.
UBlueprint* BPDiffCT_CreateBP(const FString& PackagePath, UClass* ParentClass)
{
	const FString ObjectPath = PackagePath + TEXT(".") + FPackageName::GetShortName(PackagePath);
	if (UBlueprint* Existing = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).TryLoad()); IsValid(Existing))
	{
		return Existing;
	}

	UPackage* Package = CreatePackage(*PackagePath);
	if (!IsValid(Package))
	{
		return nullptr;
	}

	const FString AssetName = FPackageName::GetShortName(PackagePath);
	UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
		ParentClass,
		Package,
		FName(*AssetName),
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
	return BP;
}

// Add a function graph named GraphName containing exactly three nodes: the
// K2Node_FunctionEntry plus two comment nodes. Returns the graph (nullptr on
// failure).
UEdGraph* BPDiffCT_AddThreeNodeFunctionGraph(UBlueprint* BP, const TCHAR* GraphName)
{
	UEdGraph* NewGraph = FBlueprintEditorUtils::CreateNewGraph(
		BP, FName(GraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
	if (!IsValid(NewGraph))
	{
		return nullptr;
	}
	FBlueprintEditorUtils::AddFunctionGraph<UClass>(
		BP, NewGraph, /*bIsUserCreated=*/true, static_cast<UClass*>(nullptr));

	for (int32 i = 0; i < 2; ++i)
	{
		UEdGraphNode_Comment* Comment = NewObject<UEdGraphNode_Comment>(NewGraph);
		Comment->SetFlags(RF_Transactional);
		Comment->NodePosX = 200 * i;
		Comment->NodePosY = 300;
		NewGraph->AddNode(Comment, /*bUserAction=*/false, /*bSelectNewNode=*/false);
		Comment->CreateNewGuid();
	}
	return NewGraph;
}

TSharedPtr<FJsonObject> BPDiffCT_MakeArgs(const FString& PathA, const FString& PathB)
{
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path_a"), PathA);
	Args->SetStringField(TEXT("asset_path_b"), PathB);
	return Args;
}

void BPDiffCT_SetSections(const TSharedPtr<FJsonObject>& Args, const TArray<FString>& Sections)
{
	TArray<TSharedPtr<FJsonValue>> SectionValues;
	for (const FString& Sec : Sections)
	{
		SectionValues.Add(MakeShared<FJsonValueString>(Sec));
	}
	Args->SetArrayField(TEXT("sections"), SectionValues);
}

void BPDiffCT_SetPropertyFilter(const TSharedPtr<FJsonObject>& Args, const TArray<FString>& Names)
{
	TArray<TSharedPtr<FJsonValue>> FilterValues;
	for (const FString& FilterName : Names)
	{
		FilterValues.Add(MakeShared<FJsonValueString>(FilterName));
	}
	Args->SetArrayField(TEXT("property_filter"), FilterValues);
}

// Read an integer field from a JSON object; returns -1 when absent so a missing
// field never masquerades as a passing zero.
int32 BPDiffCT_GetInt(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName)
{
	int32 Value = -1;
	if (!Obj.IsValid() || !Obj->TryGetNumberField(FieldName, Value))
	{
		return -1;
	}
	return Value;
}

} // namespace ClaireonBlueprintDiffContractTestsInternal

using namespace ClaireonBlueprintDiffContractTestsInternal;

// ============================================================================
// Whole-graph adds count their contained nodes
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BlueprintDiffContract, DefaultSections_WholeGraphAddCountsContainedNodes, UNTEST_TIMEOUTMS(60000))
{
	const FString PathA = TEXT("/Game/__MCPTests/BP_DiffCT_A1");
	const FString PathB = TEXT("/Game/__MCPTests/BP_DiffCT_B1");
	BPDiffCT_Cleanup(PathA);
	BPDiffCT_Cleanup(PathB);

	UBlueprint* BPA = BPDiffCT_CreateBP(PathA, AActor::StaticClass());
	UBlueprint* BPB = BPDiffCT_CreateBP(PathB, AActor::StaticClass());
	UNTEST_ASSERT_PTR(BPA);
	UNTEST_ASSERT_PTR(BPB);

	UEdGraph* ExtraGraph = BPDiffCT_AddThreeNodeFunctionGraph(BPB, TEXT("DiffContractFunc"));
	UNTEST_ASSERT_PTR(ExtraGraph);
	UNTEST_ASSERT_EQ(ExtraGraph->Nodes.Num(), 3);

	ClaireonTool_BlueprintDiff Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(BPDiffCT_MakeArgs(PathA, PathB));
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BlueprintDiffContract] Error: %s"), *Result.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	// The whole-graph addition must count its 3 contained nodes, not 1.
	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(Result.Data, TEXT("nodes_added")), 3);
	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(Result.Data, TEXT("graphs_added")), 1);
	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(Result.Data, TEXT("graphs_removed")), 0);

	bool bDiffers = false;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetBoolField(TEXT("differs"), bDiffers));
	UNTEST_EXPECT_TRUE(bDiffers);

	BPDiffCT_Cleanup(PathA);
	BPDiffCT_Cleanup(PathB);
	co_return;
}

// ============================================================================
// sections=['cdo'] produces a real CDO verdict, never a bare no-diff claim
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BlueprintDiffContract, SectionsCdo_ReportsCdoDelta_NotSilentNoDiff, UNTEST_TIMEOUTMS(60000))
{
	const FString PathA = TEXT("/Game/__MCPTests/BP_DiffCT_PawnA2");
	const FString PathB = TEXT("/Game/__MCPTests/BP_DiffCT_ActorB2");
	BPDiffCT_Cleanup(PathA);
	BPDiffCT_Cleanup(PathB);

	// Different parent classes guarantee CDO property deltas (Pawn-only
	// properties are missing from the Actor side).
	UBlueprint* BPA = BPDiffCT_CreateBP(PathA, APawn::StaticClass());
	UBlueprint* BPB = BPDiffCT_CreateBP(PathB, AActor::StaticClass());
	UNTEST_ASSERT_PTR(BPA);
	UNTEST_ASSERT_PTR(BPB);

	TSharedPtr<FJsonObject> Args = BPDiffCT_MakeArgs(PathA, PathB);
	BPDiffCT_SetSections(Args, {TEXT("cdo")});

	ClaireonTool_BlueprintDiff Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BlueprintDiffContract] Error: %s"), *Result.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	const TSharedPtr<FJsonObject>* CDOObj = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("cdo"), CDOObj));
	UNTEST_EXPECT_GT(BPDiffCT_GetInt(*CDOObj, TEXT("fields_changed")), 0);

	// Two genuinely different Blueprints must never read as "no differences".
	bool bDiffers = false;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetBoolField(TEXT("differs"), bDiffers));
	UNTEST_EXPECT_TRUE(bDiffers);

	// The summary must report the cdo verdict, not a misleading "0 nodes" claim.
	UNTEST_EXPECT_TRUE(Result.Summary.Contains(TEXT("cdo:")));
	UNTEST_EXPECT_FALSE(Result.Summary.Contains(TEXT("nodes added")));

	BPDiffCT_Cleanup(PathA);
	BPDiffCT_Cleanup(PathB);
	co_return;
}

// ============================================================================
// Identical assets: every counter zero
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BlueprintDiffContract, IdenticalAssets_AllCountersZero, UNTEST_TIMEOUTMS(60000))
{
	const FString PathA = TEXT("/Game/__MCPTests/BP_DiffCT_IdA3");
	const FString PathB = TEXT("/Game/__MCPTests/BP_DiffCT_IdB3");
	BPDiffCT_Cleanup(PathA);
	BPDiffCT_Cleanup(PathB);

	UBlueprint* BPA = BPDiffCT_CreateBP(PathA, AActor::StaticClass());
	UBlueprint* BPB = BPDiffCT_CreateBP(PathB, AActor::StaticClass());
	UNTEST_ASSERT_PTR(BPA);
	UNTEST_ASSERT_PTR(BPB);

	ClaireonTool_BlueprintDiff Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(BPDiffCT_MakeArgs(PathA, PathB));
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BlueprintDiffContract] Error: %s"), *Result.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(Result.Data, TEXT("nodes_added")), 0);
	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(Result.Data, TEXT("nodes_removed")), 0);
	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(Result.Data, TEXT("nodes_changed")), 0);
	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(Result.Data, TEXT("connections_changed")), 0);
	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(Result.Data, TEXT("graphs_added")), 0);
	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(Result.Data, TEXT("graphs_removed")), 0);

	const TSharedPtr<FJsonObject>* SCSObj = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("scs"), SCSObj));
	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(*SCSObj, TEXT("components_added")), 0);
	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(*SCSObj, TEXT("components_removed")), 0);
	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(*SCSObj, TEXT("components_changed")), 0);

	const TSharedPtr<FJsonObject>* CDOObj = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("cdo"), CDOObj));
	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(*CDOObj, TEXT("fields_changed")), 0);

	bool bDiffers = true; // opposite start catches a missing field
	UNTEST_ASSERT_TRUE(Result.Data->TryGetBoolField(TEXT("differs"), bDiffers));
	UNTEST_EXPECT_FALSE(bDiffers);

	BPDiffCT_Cleanup(PathA);
	BPDiffCT_Cleanup(PathB);
	co_return;
}

// ============================================================================
// Section selection errors (no silent no-diff verdicts)
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BlueprintDiffContract, UnknownSection_Errors, UNTEST_TIMEOUTMS(10000))
{
	// Parameter validation fires before asset loading, so no fixtures needed.
	TSharedPtr<FJsonObject> Args = BPDiffCT_MakeArgs(
		TEXT("/Game/__MCPTests/DiffCT_NoSuchA"), TEXT("/Game/__MCPTests/DiffCT_NoSuchB"));
	BPDiffCT_SetSections(Args, {TEXT("bogus")});

	ClaireonTool_BlueprintDiff Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_STREQ(*Result.ErrorMessage,
		TEXT("Unknown section 'bogus'. Valid sections: 'graphs', 'cdo', 'scs'."));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintDiffContract, EmptySections_Errors, UNTEST_TIMEOUTMS(10000))
{
	TSharedPtr<FJsonObject> Args = BPDiffCT_MakeArgs(
		TEXT("/Game/__MCPTests/DiffCT_NoSuchA"), TEXT("/Game/__MCPTests/DiffCT_NoSuchB"));
	BPDiffCT_SetSections(Args, {});

	ClaireonTool_BlueprintDiff Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_STREQ(*Result.ErrorMessage,
		TEXT("'sections' is empty: nothing would be diffed. Specify at least one of 'graphs', 'cdo', 'scs', or omit the field to diff all three."));
	co_return;
}

// ============================================================================
// property_filter contract
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BlueprintDiffContract, PropertyFilter_RestrictsCdoDiff, UNTEST_TIMEOUTMS(60000))
{
	const FString PathA = TEXT("/Game/__MCPTests/BP_DiffCT_PawnA6");
	const FString PathB = TEXT("/Game/__MCPTests/BP_DiffCT_ActorB6");
	BPDiffCT_Cleanup(PathA);
	BPDiffCT_Cleanup(PathB);

	UBlueprint* BPA = BPDiffCT_CreateBP(PathA, APawn::StaticClass());
	UBlueprint* BPB = BPDiffCT_CreateBP(PathB, AActor::StaticClass());
	UNTEST_ASSERT_PTR(BPA);
	UNTEST_ASSERT_PTR(BPB);

	// A filter naming no real property restricts the cdo diff to nothing, so
	// the (otherwise different) CDOs report zero changed fields in-scope.
	TSharedPtr<FJsonObject> Args = BPDiffCT_MakeArgs(PathA, PathB);
	BPDiffCT_SetSections(Args, {TEXT("cdo")});
	BPDiffCT_SetPropertyFilter(Args, {TEXT("ThisPropertyDoesNotExist_XYZ")});

	ClaireonTool_BlueprintDiff Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BlueprintDiffContract] Error: %s"), *Result.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	const TSharedPtr<FJsonObject>* CDOObj = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetObjectField(TEXT("cdo"), CDOObj));
	UNTEST_EXPECT_EQ(BPDiffCT_GetInt(*CDOObj, TEXT("fields_changed")), 0);

	bool bDiffers = true;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetBoolField(TEXT("differs"), bDiffers));
	UNTEST_EXPECT_FALSE(bDiffers);

	BPDiffCT_Cleanup(PathA);
	BPDiffCT_Cleanup(PathB);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, BlueprintDiffContract, PropertyFilter_WithoutCdoOrScs_Errors, UNTEST_TIMEOUTMS(10000))
{
	TSharedPtr<FJsonObject> Args = BPDiffCT_MakeArgs(
		TEXT("/Game/__MCPTests/DiffCT_NoSuchA"), TEXT("/Game/__MCPTests/DiffCT_NoSuchB"));
	BPDiffCT_SetSections(Args, {TEXT("graphs")});
	BPDiffCT_SetPropertyFilter(Args, {TEXT("SomeProperty")});

	ClaireonTool_BlueprintDiff Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	UNTEST_ASSERT_TRUE(Result.bIsError);
	UNTEST_EXPECT_STREQ(*Result.ErrorMessage,
		TEXT("'property_filter' only applies to the 'cdo' and 'scs' sections, but neither is selected in 'sections'."));
	co_return;
}

// ============================================================================
// resolution='detailed' whole-graph entries carry node_count
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, BlueprintDiffContract, DetailedResolution_WholeGraphAddCarriesNodeCount, UNTEST_TIMEOUTMS(60000))
{
	const FString PathA = TEXT("/Game/__MCPTests/BP_DiffCT_A8");
	const FString PathB = TEXT("/Game/__MCPTests/BP_DiffCT_B8");
	BPDiffCT_Cleanup(PathA);
	BPDiffCT_Cleanup(PathB);

	UBlueprint* BPA = BPDiffCT_CreateBP(PathA, AActor::StaticClass());
	UBlueprint* BPB = BPDiffCT_CreateBP(PathB, AActor::StaticClass());
	UNTEST_ASSERT_PTR(BPA);
	UNTEST_ASSERT_PTR(BPB);

	UEdGraph* ExtraGraph = BPDiffCT_AddThreeNodeFunctionGraph(BPB, TEXT("DiffContractFunc8"));
	UNTEST_ASSERT_PTR(ExtraGraph);

	TSharedPtr<FJsonObject> Args = BPDiffCT_MakeArgs(PathA, PathB);
	Args->SetStringField(TEXT("resolution"), TEXT("detailed"));
	BPDiffCT_SetSections(Args, {TEXT("graphs")});

	ClaireonTool_BlueprintDiff Tool;
	IClaireonTool::FToolResult Result = Tool.Execute(Args);
	if (Result.bIsError)
	{
		UE_LOG(LogTemp, Error, TEXT("[BlueprintDiffContract] Error: %s"), *Result.ErrorMessage);
	}
	UNTEST_ASSERT_FALSE(Result.bIsError);
	UNTEST_ASSERT_TRUE(Result.Data.IsValid());

	const TArray<TSharedPtr<FJsonValue>>* GraphsArray = nullptr;
	UNTEST_ASSERT_TRUE(Result.Data->TryGetArrayField(TEXT("graphs"), GraphsArray));

	bool bFoundAddedEntry = false;
	int32 AddedEntryNodeCount = -1;
	for (const TSharedPtr<FJsonValue>& EntryVal : *GraphsArray)
	{
		if (!EntryVal.IsValid() || EntryVal->Type != EJson::Object)
		{
			continue;
		}
		const TSharedPtr<FJsonObject>& Entry = EntryVal->AsObject();
		FString GraphName;
		FString Status;
		Entry->TryGetStringField(TEXT("graph"), GraphName);
		Entry->TryGetStringField(TEXT("status"), Status);
		if (GraphName == TEXT("DiffContractFunc8") && Status == TEXT("added"))
		{
			bFoundAddedEntry = true;
			AddedEntryNodeCount = BPDiffCT_GetInt(Entry, TEXT("node_count"));
			break;
		}
	}
	UNTEST_EXPECT_TRUE(bFoundAddedEntry);
	UNTEST_EXPECT_EQ(AddedEntryNodeCount, 3);

	BPDiffCT_Cleanup(PathA);
	BPDiffCT_Cleanup(PathB);
	co_return;
}

#endif // WITH_UNTESTED
