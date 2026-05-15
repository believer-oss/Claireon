// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintDiff.h"
#include "ClaireonBlueprintHelpers.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "K2Node_CallFunction.h"
#include "K2Node_IfThenElse.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"

namespace ClaireonBlueprintDiffSpec
{
	/** Create a throwaway Blueprint under /Game/__MCPTests. Returns nullptr on failure. */
	static UBlueprint* CreateThrowawayBP(const FString& AssetName)
	{
		const FString PackageName = FString::Printf(TEXT("/Game/__MCPTests/%s"), *AssetName);
		const FString FilePath = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		if (FPaths::FileExists(FilePath))
		{
			IFileManager::Get().Delete(*FilePath, false, true);
		}
		UPackage* Package = CreatePackage(*PackageName);
		if (!Package)
		{
			return nullptr;
		}
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);
		if (BP)
		{
			FAssetRegistryModule::AssetCreated(BP);
		}
		return BP;
	}

	/** Add a plain Branch node to the EventGraph. Returns the new node (or nullptr). */
	static UEdGraphNode* AddBranchNode(UBlueprint* BP)
	{
		UEdGraph* Graph = ClaireonBlueprintHelpers::FindGraphByName(BP, TEXT("EventGraph"));
		if (!Graph)
		{
			return nullptr;
		}
		UK2Node_IfThenElse* BranchNode = NewObject<UK2Node_IfThenElse>(Graph);
		BranchNode->CreateNewGuid();
		BranchNode->PostPlacedNewNode();
		BranchNode->AllocateDefaultPins();
		Graph->AddNode(BranchNode, true, false);
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		return BranchNode;
	}

	/** Parse the diff text and return the bullet count under a named section. Returns -1 if section missing. */
	static int32 CountBulletsInSection(const FString& DiffText, const FString& Header)
	{
		int32 HeaderPos = DiffText.Find(Header);
		if (HeaderPos == INDEX_NONE)
		{
			return -1;
		}
		int32 StartPos = HeaderPos + Header.Len();
		int32 NextHeader = DiffText.Find(TEXT("### "), ESearchCase::IgnoreCase, ESearchDir::FromStart, StartPos);
		const FString Section = (NextHeader == INDEX_NONE)
			? DiffText.RightChop(StartPos)
			: DiffText.Mid(StartPos, NextHeader - StartPos);
		// "- (none)" means empty section (zero real bullets).
		if (Section.Contains(TEXT("- (none)")))
		{
			return 0;
		}
		int32 Count = 0;
		int32 ScanPos = 0;
		while (true)
		{
			int32 Found = Section.Find(TEXT("\n- "), ESearchCase::CaseSensitive, ESearchDir::FromStart, ScanPos);
			if (Found == INDEX_NONE) break;
			++Count;
			ScanPos = Found + 1;
		}
		// Catch a leading bullet on the first line of the section (no newline prefix).
		if (Section.StartsWith(TEXT("- ")) && !Section.StartsWith(TEXT("- (none)")))
		{
			++Count;
		}
		return Count;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDiffSpec_EmptySpec_MatchesEmptyGraph,
	"Claireon.BlueprintDiff.EmptySpecMatchesEmptyGraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDiffSpec_EmptySpec_MatchesEmptyGraph::RunTest(const FString& Parameters)
{
	using namespace ClaireonBlueprintDiffSpec;

	UBlueprint* BP = CreateThrowawayBP(TEXT("BP_DiffSpec_PositivePath"));
	if (!TestNotNull(TEXT("Blueprint created"), BP)) return false;

	// Spec declares a single Branch node, and we'll add the same Branch to the asset.
	// Default Event nodes on a fresh Actor BP are expected to surface under Nodes removed
	// (since the spec doesn't enumerate them); we only assert that the spec-declared
	// Branch is NOT listed under Nodes added.
	AddBranchNode(BP);

	ClaireonTool_BlueprintDiff Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path_a"), BP->GetPathName());
	Args->SetStringField(TEXT("spec_json"),
		TEXT("{\"nodes\":[{\"id\":\"n1\",\"type\":\"Branch\"}]}"));

	auto Result = Tool.Execute(Args);
	TestFalse(TEXT("Result is not error"), Result.bIsError);
	TestTrue(TEXT("Data is set"), Result.Data.IsValid());

	FString Diff;
	Result.Data->TryGetStringField(TEXT("diff"), Diff);
	TestTrue(TEXT("Nodes added section present"), Diff.Contains(TEXT("### Nodes added")));
	TestTrue(TEXT("Nodes removed section present"), Diff.Contains(TEXT("### Nodes removed")));
	TestTrue(TEXT("Nodes changed section present"), Diff.Contains(TEXT("### Nodes changed")));
	TestTrue(TEXT("Connections added section present"), Diff.Contains(TEXT("### Connections added")));
	TestTrue(TEXT("Connections removed section present"), Diff.Contains(TEXT("### Connections removed")));
	TestTrue(TEXT("Variables section present"), Diff.Contains(TEXT("### Variables")));

	double NodesAdded = 0.0;
	Result.Data->TryGetNumberField(TEXT("nodes_added"), NodesAdded);
	TestEqual(TEXT("Spec's Branch matched asset's Branch -> nodes_added == 0"),
		static_cast<int32>(NodesAdded), 0);
	TestEqual(TEXT("Nodes added bullet count"),
		CountBulletsInSection(Diff, TEXT("### Nodes added")), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDiffSpec_NodeInSpecNotInAsset_AppearsAsAdded,
	"Claireon.BlueprintDiff.NodeAddedDetection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDiffSpec_NodeInSpecNotInAsset_AppearsAsAdded::RunTest(const FString& Parameters)
{
	using namespace ClaireonBlueprintDiffSpec;

	UBlueprint* BP = CreateThrowawayBP(TEXT("BP_DiffSpec_NodeAdded"));
	if (!TestNotNull(TEXT("Blueprint created"), BP)) return false;

	ClaireonTool_BlueprintDiff Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path_a"), BP->GetPathName());
	// Spec declares a CallFunction(PrintString) that the fresh BP does not have.
	Args->SetStringField(TEXT("spec_json"),
		TEXT("{\"nodes\":[{\"id\":\"n1\",\"type\":\"CallFunction\",\"function\":\"PrintString\"}]}"));

	auto Result = Tool.Execute(Args);
	TestFalse(TEXT("Result is not error"), Result.bIsError);

	double NodesAdded = 0.0;
	Result.Data->TryGetNumberField(TEXT("nodes_added"), NodesAdded);
	TestEqual(TEXT("nodes_added count"), static_cast<int32>(NodesAdded), 1);

	FString Diff;
	Result.Data->TryGetStringField(TEXT("diff"), Diff);
	TestTrue(TEXT("Diff mentions PrintString under Added"),
		Diff.Contains(TEXT("CallFunction(PrintString)")));
	TestEqual(TEXT("Nodes added bullet count"),
		CountBulletsInSection(Diff, TEXT("### Nodes added")), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDiffSpec_NodeInAssetNotInSpec_AppearsAsRemoved,
	"Claireon.BlueprintDiff.NodeRemovedDetection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDiffSpec_NodeInAssetNotInSpec_AppearsAsRemoved::RunTest(const FString& Parameters)
{
	using namespace ClaireonBlueprintDiffSpec;

	UBlueprint* BP = CreateThrowawayBP(TEXT("BP_DiffSpec_NodeRemoved"));
	if (!TestNotNull(TEXT("Blueprint created"), BP)) return false;

	// Asset has a Branch; spec is empty (just variables section to keep spec non-trivial).
	AddBranchNode(BP);

	ClaireonTool_BlueprintDiff Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path_a"), BP->GetPathName());
	Args->SetStringField(TEXT("spec_json"), TEXT("{\"nodes\":[]}"));

	auto Result = Tool.Execute(Args);
	TestFalse(TEXT("Result is not error"), Result.bIsError);

	double NodesRemoved = 0.0;
	Result.Data->TryGetNumberField(TEXT("nodes_removed"), NodesRemoved);
	TestTrue(TEXT("nodes_removed >= 1 (Branch)"), NodesRemoved >= 1.0);

	FString Diff;
	Result.Data->TryGetStringField(TEXT("diff"), Diff);
	TestTrue(TEXT("Diff mentions IfThenElse under Removed"),
		Diff.Contains(TEXT("IfThenElse")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDiffSpec_MalformedSpec_ReturnsStructuredError,
	"Claireon.BlueprintDiff.MalformedSpecError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDiffSpec_MalformedSpec_ReturnsStructuredError::RunTest(const FString& Parameters)
{
	using namespace ClaireonBlueprintDiffSpec;

	UBlueprint* BP = CreateThrowawayBP(TEXT("BP_DiffSpec_Malformed"));
	if (!TestNotNull(TEXT("Blueprint created"), BP)) return false;

	ClaireonTool_BlueprintDiff Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path_a"), BP->GetPathName());
	// Missing closing brace -> parse failure.
	Args->SetStringField(TEXT("spec_json"), TEXT("{\"nodes\":[{\"id\":\"n1\""));

	auto Result = Tool.Execute(Args);
	TestTrue(TEXT("Result is error"), Result.bIsError);
	TestTrue(TEXT("Error message cites spec_json parse"),
		Result.ErrorMessage.Contains(TEXT("spec_json parse failed")));

	return true;
}
