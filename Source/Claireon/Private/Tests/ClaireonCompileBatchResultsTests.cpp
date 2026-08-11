// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for claireon.bp_compile_batch compiler-message reporting (WI-6).
//
// Historically the tool called CompileBlueprint without an FCompilerResultsLog
// and hardcoded errors:[]/warnings:[] per entry, so failed compiles returned
// status='failed' with no message text. These tests pin the fixed contract:
// a deliberately broken Blueprint (call to a nonexistent -- i.e. deleted --
// function) must report status='failed' with non-empty error text naming the
// missing function, and a healthy Blueprint must report status='succeeded'
// with empty errors/warnings.
//
// Creates throwaway Blueprints under /Game/__MCPTests/, drives the tool
// through its JSON Execute entry, and deletes the assets in teardown.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonTool_BlueprintCompileBatch.h"
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
#include "K2Node_CustomEvent.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonCompileBatchResultsTestsInternal
{
	static const TCHAR* CompileBatchTestBPPath_Broken  = TEXT("/Game/__MCPTests/BP_CompileBatchResults_Broken");
	static const TCHAR* CompileBatchTestBPPath_Healthy = TEXT("/Game/__MCPTests/BP_CompileBatchResults_Healthy");

	// Name of the function the broken node references after we simulate its
	// deletion. Must not exist on UKismetSystemLibrary.
	static const TCHAR* CompileBatchTestDeletedFunctionName = TEXT("ClaireonThisFunctionWasDeleted");

	void CompileBatchTestCleanupAsset(const FString& AssetPath)
	{
		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> AssetsToDelete;
			AssetsToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
		}
	}

	UBlueprint* CompileBatchTestCreateActorBP(const FString& AssetPath)
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

	UEdGraph* CompileBatchTestFindEventGraph(UBlueprint* BP)
	{
		for (UEdGraph* G : BP->UbergraphPages)
		{
			if (IsValid(G)) return G;
		}
		return nullptr;
	}

	// Wire a custom event (a compile root that survives node pruning) into a
	// CallFunction node bound to a real function, then rebind the call to a
	// function name that does not exist -- simulating a call to a deleted
	// function. Compiling the Blueprint afterwards must fail with an error
	// message naming CompileBatchTestDeletedFunctionName.
	bool CompileBatchTestBreakBlueprint(UBlueprint* BP)
	{
		UEdGraph* EventGraph = CompileBatchTestFindEventGraph(BP);
		if (!IsValid(EventGraph)) return false;

		UK2Node_CustomEvent* EventNode = NewObject<UK2Node_CustomEvent>(EventGraph);
		if (!IsValid(EventNode)) return false;
		EventNode->CustomFunctionName = FName(TEXT("ClaireonCompileBatchTestEvent"));
		EventNode->SetFlags(RF_Transactional);
		EventGraph->AddNode(EventNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
		EventNode->CreateNewGuid();
		EventNode->PostPlacedNewNode();
		EventNode->AllocateDefaultPins();

		// Bind to a real function first so AllocateDefaultPins creates the exec
		// pins we need for wiring; the reference is broken afterwards.
		UFunction* PrintStringFn = UKismetSystemLibrary::StaticClass()->FindFunctionByName(TEXT("PrintString"));
		if (!IsValid(PrintStringFn)) return false;

		UK2Node_CallFunction* CallNode = NewObject<UK2Node_CallFunction>(EventGraph);
		if (!IsValid(CallNode)) return false;
		CallNode->SetFromFunction(PrintStringFn);
		CallNode->SetFlags(RF_Transactional);
		EventGraph->AddNode(CallNode, /*bUserAction=*/false, /*bSelectNewNode=*/false);
		CallNode->CreateNewGuid();
		CallNode->PostPlacedNewNode();
		CallNode->AllocateDefaultPins();

		UEdGraphPin* ThenPin = EventNode->FindPin(UEdGraphSchema_K2::PN_Then, EGPD_Output);
		UEdGraphPin* ExecPin = CallNode->GetExecPin();
		if (!ThenPin || !ExecPin) return false;
		ThenPin->MakeLinkTo(ExecPin);

		// Simulate the referenced function having been deleted.
		CallNode->FunctionReference.SetExternalMember(
			FName(CompileBatchTestDeletedFunctionName), UKismetSystemLibrary::StaticClass());

		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
		return true;
	}

	TSharedPtr<FJsonObject> CompileBatchTestMakeArgs(const TCHAR* AssetPath)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Paths;
		Paths.Add(MakeShared<FJsonValueString>(AssetPath));
		Args->SetArrayField(TEXT("paths"), Paths);
		return Args;
	}

	// Fetch results[Index] out of the tool's Data payload. Returns null on any
	// shape mismatch so the caller can assert (asserts cannot live here: this
	// is called from test bodies that own the co_return-expanding macros).
	TSharedPtr<FJsonObject> CompileBatchTestGetResultEntry(const TSharedPtr<FJsonObject>& Data, int32 Index)
	{
		if (!Data.IsValid()) return nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Results = nullptr;
		if (!Data->TryGetArrayField(TEXT("results"), Results)) return nullptr;
		if (!Results->IsValidIndex(Index)) return nullptr;
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		if (!(*Results)[Index]->TryGetObject(Entry)) return nullptr;
		return *Entry;
	}

	// Collect a string array field into OutStrings; false on shape mismatch.
	bool CompileBatchTestGetStringArray(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName, TArray<FString>& OutStrings)
	{
		OutStrings.Reset();
		if (!Obj.IsValid()) return false;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Obj->TryGetArrayField(FieldName, Values)) return false;
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (!Value->TryGetString(Text)) return false;
			OutStrings.Add(Text);
		}
		return true;
	}
} // namespace ClaireonCompileBatchResultsTestsInternal

using namespace ClaireonCompileBatchResultsTestsInternal;

// ============================================================================
// Test 1: A Blueprint with a call to a deleted (nonexistent) function compiles
// to status='failed' AND carries the compiler's error text -- errors[] must be
// non-empty, every entry non-empty, and the text must name the missing
// function. Pins the fix for the hardcoded-empty errors[]/warnings[] defect.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, CompileBatchResults, Functional_BrokenBlueprintReportsCompilerErrors, UNTEST_TIMEOUTMS(120000))
{
	CompileBatchTestCleanupAsset(CompileBatchTestBPPath_Broken);
	UBlueprint* BP = CompileBatchTestCreateActorBP(CompileBatchTestBPPath_Broken);
	UNTEST_ASSERT_PTR(BP);
	UNTEST_ASSERT_TRUE(CompileBatchTestBreakBlueprint(BP));

	ClaireonTool_BlueprintCompileBatch Tool;
	IClaireonTool::FToolResult R = Tool.Execute(CompileBatchTestMakeArgs(CompileBatchTestBPPath_Broken));

	// The batch call itself succeeds; the failure is reported per entry.
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_VALID(R.Data);
	UNTEST_EXPECT_EQ(static_cast<int32>(R.Data->GetNumberField(TEXT("total"))), 1);
	UNTEST_EXPECT_EQ(static_cast<int32>(R.Data->GetNumberField(TEXT("succeeded"))), 0);
	UNTEST_EXPECT_EQ(static_cast<int32>(R.Data->GetNumberField(TEXT("failed"))), 1);

	TSharedPtr<FJsonObject> Entry = CompileBatchTestGetResultEntry(R.Data, 0);
	UNTEST_ASSERT_VALID(Entry);
	UNTEST_ASSERT_EQ(Entry->GetStringField(TEXT("status")), FString(TEXT("failed")));

	TArray<FString> ErrorTexts;
	UNTEST_ASSERT_TRUE(CompileBatchTestGetStringArray(Entry, TEXT("errors"), ErrorTexts));
	UNTEST_ASSERT_GT(ErrorTexts.Num(), 0);
	UNTEST_EXPECT_EQ(static_cast<int32>(Entry->GetNumberField(TEXT("error_count"))), ErrorTexts.Num());

	FString AllErrorText;
	for (const FString& ErrorText : ErrorTexts)
	{
		UNTEST_EXPECT_FALSE(ErrorText.TrimStartAndEnd().IsEmpty());
		AllErrorText += ErrorText + TEXT("\n");
	}
	// The compiler's "could not find function" message names the deleted
	// function, so the payload must carry it -- this is exactly the text the
	// old hardcoded-empty errors[] threw away.
	UNTEST_EXPECT_TRUE(AllErrorText.Contains(CompileBatchTestDeletedFunctionName));

	CompileBatchTestCleanupAsset(CompileBatchTestBPPath_Broken);
	co_return;
}

// ============================================================================
// Test 2: A healthy Blueprint reports status='succeeded' with empty errors[]
// and warnings[]. (The doc's shorthand says status='ok'; the tool's shipped
// wire contract is 'succeeded'/'failed' and existing callers depend on it, so
// the test pins the real contract.)
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, CompileBatchResults, Functional_HealthyBlueprintReportsSuccessWithNoErrors, UNTEST_TIMEOUTMS(120000))
{
	CompileBatchTestCleanupAsset(CompileBatchTestBPPath_Healthy);
	UBlueprint* BP = CompileBatchTestCreateActorBP(CompileBatchTestBPPath_Healthy);
	UNTEST_ASSERT_PTR(BP);

	ClaireonTool_BlueprintCompileBatch Tool;
	IClaireonTool::FToolResult R = Tool.Execute(CompileBatchTestMakeArgs(CompileBatchTestBPPath_Healthy));

	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_VALID(R.Data);
	UNTEST_EXPECT_EQ(static_cast<int32>(R.Data->GetNumberField(TEXT("total"))), 1);
	UNTEST_EXPECT_EQ(static_cast<int32>(R.Data->GetNumberField(TEXT("succeeded"))), 1);
	UNTEST_EXPECT_EQ(static_cast<int32>(R.Data->GetNumberField(TEXT("failed"))), 0);

	TSharedPtr<FJsonObject> Entry = CompileBatchTestGetResultEntry(R.Data, 0);
	UNTEST_ASSERT_VALID(Entry);
	UNTEST_ASSERT_EQ(Entry->GetStringField(TEXT("status")), FString(TEXT("succeeded")));

	TArray<FString> ErrorTexts;
	UNTEST_ASSERT_TRUE(CompileBatchTestGetStringArray(Entry, TEXT("errors"), ErrorTexts));
	UNTEST_EXPECT_EQ(ErrorTexts.Num(), 0);

	TArray<FString> WarningTexts;
	UNTEST_ASSERT_TRUE(CompileBatchTestGetStringArray(Entry, TEXT("warnings"), WarningTexts));
	UNTEST_EXPECT_EQ(WarningTexts.Num(), 0);

	CompileBatchTestCleanupAsset(CompileBatchTestBPPath_Healthy);
	co_return;
}

#endif // WITH_UNTESTED
