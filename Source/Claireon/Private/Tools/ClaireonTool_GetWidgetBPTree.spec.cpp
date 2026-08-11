// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// MVVM setup helpers use the decomposed ClaireonWidgetBPTool_* classes
// instead of the deleted ClaireonTool_EditWidgetBP shim. The tool-under-test
// (ClaireonTool_GetWidgetBPTree) is untouched.

#include "Misc/AutomationTest.h"
#include "Tools/ClaireonTool_GetWidgetBPTree.h"
#include "Tools/ClaireonWidgetBPTool_Create.h"
#include "Tools/ClaireonWidgetBPTool_AddMVVMViewModel.h"
#include "Tools/ClaireonWidgetBPTool_Compile.h"
#include "Tools/ClaireonWidgetBPTool_Save.h"
#include "Tools/ClaireonWidgetBPTool_Close.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/UserWidget.h"
#include "Components/CanvasPanel.h"
#include "Components/TextBlock.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetWidgetBPTreeTest_BasicRead,
	"Claireon.GetWidgetBPTree.BasicRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGetWidgetBPTreeTest_BasicRead::RunTest(const FString& Parameters)
{
	const FString PackageName = TEXT("/Game/__MCPTests/WBP_ReadTest");
	const FString AssetName = TEXT("WBP_ReadTest");

	// --- Setup: delete any leftover asset from a prior failed run ---
	{
		UPackage* ExistingPkg = FindPackage(nullptr, *PackageName);
		if (IsValid(ExistingPkg))
		{
			TArray<UObject*> ObjectsInPkg;
			GetObjectsWithOuter(ExistingPkg, ObjectsInPkg, false);
			for (UObject* Obj : ObjectsInPkg)
			{
				if (IsValid(Obj))
				{
					Obj->ClearFlags(RF_Standalone);
				}
			}
			ExistingPkg->ClearFlags(RF_Standalone);
		}
	}

	// --- Create a test Widget Blueprint with a CanvasPanel root and a TextBlock child ---
	UPackage* Package = CreatePackage(*PackageName);
	if (!IsValid(Package))
	{
		AddError(TEXT("Failed to create test package"));
		return false;
	}

	UWidgetBlueprint* WBP = CastChecked<UWidgetBlueprint>(
		FKismetEditorUtilities::CreateBlueprint(
			UUserWidget::StaticClass(),
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None));

	if (!IsValid(WBP))
	{
		AddError(TEXT("Failed to create test Widget Blueprint"));
		return false;
	}

	if (!WBP->WidgetTree)
	{
		AddError(TEXT("Widget Blueprint has no WidgetTree"));
		return false;
	}

	// Build a simple hierarchy: CanvasPanel (root) -> TextBlock
	UCanvasPanel* Root = WBP->WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("TestCanvas"));
	if (!IsValid(Root))
	{
		AddError(TEXT("Failed to create root CanvasPanel"));
		return false;
	}
	WBP->WidgetTree->RootWidget = Root;

	UTextBlock* Text = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("TestText"));
	if (!IsValid(Text))
	{
		AddError(TEXT("Failed to create TestText TextBlock"));
		return false;
	}
	Root->AddChild(Text);

	// Notify asset registry so LoadObject can find the in-memory asset
	FAssetRegistryModule::AssetCreated(WBP);
	Package->MarkPackageDirty();

	// --- Execute the tool ---
	ClaireonTool_GetWidgetBPTree Tool;

	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("asset_path"), PackageName);

	auto Result = Tool.Execute(Args);

	if (Result.bIsError)
	{
		AddError(FString::Printf(TEXT("GetWidgetBPTree returned an error: %s"), *Result.GetContentAsString()));
		return false;
	}

	AddInfo(TEXT("GetWidgetBPTree succeeded without error"));

	// --- Read the structured payload off Result.Data ---
	// This used to parse Result.GetContentAsString() as JSON and look for a top-level
	// "root". GetContentAsString() returns ErrorMessage-or-Summary and never touches
	// Data, and this tool's success Summary is prose ("<asset>: N widgets (root: X)"),
	// so the parse could never succeed: the whole block below was unreachable and the
	// test only ever proved "the tool did not set bIsError". The tree lives in
	// Data.widget_tree (see ClaireonTool_GetWidgetBPTree::Execute), so read it from there.
	TestTrue("get_tree returned structured Data", Result.Data.IsValid());
	if (!Result.Data.IsValid())
	{
		return false;
	}

	int32 ReportedWidgetCount = 0;
	{
		double WidgetCountNum = 0.0;
		TestTrue("Data should contain 'widget_count'",
			Result.Data->TryGetNumberField(TEXT("widget_count"), WidgetCountNum));
		ReportedWidgetCount = static_cast<int32>(WidgetCountNum);
		TestTrue("widget_count should count at least the root and its child",
			ReportedWidgetCount >= 2);
	}

	FString ReportedRootClass;
	Result.Data->TryGetStringField(TEXT("root_class"), ReportedRootClass);
	TestTrue("Data root_class should be CanvasPanel", ReportedRootClass.Contains(TEXT("CanvasPanel")));

	// --- Verify Data.widget_tree is present ---
	const TSharedPtr<FJsonObject>* TreeObjPtr = nullptr;
	if (!Result.Data->TryGetObjectField(TEXT("widget_tree"), TreeObjPtr) || !TreeObjPtr || !(*TreeObjPtr).IsValid())
	{
		AddError(TEXT("Result Data is missing the 'widget_tree' object"));
		return false;
	}

	// --- Verify "root" object is present ---
	const TSharedPtr<FJsonObject>* RootObjPtr = nullptr;
	if (!(*TreeObjPtr)->TryGetObjectField(TEXT("root"), RootObjPtr) || !RootObjPtr || !(*RootObjPtr).IsValid())
	{
		AddError(TEXT("Data.widget_tree is missing the 'root' object"));
		return false;
	}
	const TSharedPtr<FJsonObject>& RootObj = *RootObjPtr;

	// --- Verify root has "name" field ---
	FString RootName;
	if (!RootObj->TryGetStringField(TEXT("name"), RootName))
	{
		AddError(TEXT("Root widget is missing 'name' field"));
		return false;
	}
	AddInfo(FString::Printf(TEXT("Root widget name: %s"), *RootName));

	// --- Verify root has "class" field ---
	FString RootClass;
	if (!RootObj->TryGetStringField(TEXT("class"), RootClass))
	{
		AddError(TEXT("Root widget is missing 'class' field"));
		return false;
	}
	AddInfo(FString::Printf(TEXT("Root widget class: %s"), *RootClass));

	TestTrue("Root class should be CanvasPanel", RootClass.Contains(TEXT("CanvasPanel")));

	// --- Verify root has "children" array containing "TestText" ---
	const TArray<TSharedPtr<FJsonValue>>* ChildrenArray = nullptr;
	if (!RootObj->TryGetArrayField(TEXT("children"), ChildrenArray) || !ChildrenArray)
	{
		AddError(TEXT("Root widget is missing 'children' array"));
		return false;
	}

	bool bFoundTestText = false;
	for (const TSharedPtr<FJsonValue>& ChildValue : *ChildrenArray)
	{
		const TSharedPtr<FJsonObject>* ChildObjPtr = nullptr;
		if (!ChildValue->TryGetObject(ChildObjPtr) || !ChildObjPtr)
		{
			continue;
		}

		FString ChildName;
		if ((*ChildObjPtr)->TryGetStringField(TEXT("name"), ChildName) && ChildName == TEXT("TestText"))
		{
			bFoundTestText = true;
			break;
		}
	}

	TestTrue("Children array should contain 'TestText'", bFoundTestText);

	// --- Cleanup: mark the in-memory objects for GC ---
	WBP->ClearFlags(RF_Standalone);
	Package->ClearFlags(RF_Standalone);

	AddInfo(TEXT("GetWidgetBPTree.BasicRead passed"));
	return true;
}

// ===========================================================================
// Test: MVVMBindingsInTree
// Verify include_mvvm_bindings flag controls MVVM data in tree output.
// ===========================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGetWidgetBPTreeTest_MVVMBindingsInTree,
	"Claireon.GetWidgetBPTree.MVVMBindingsInTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::EngineFilter)

bool FGetWidgetBPTreeTest_MVVMBindingsInTree::RunTest(const FString& Parameters)
{
	const FString AssetPath = TEXT("/Game/__MCPTests/WBP_MVVMTreeTest");

	// --- Step 1: Create WBP and add MVVM viewmodel via decomposed widget-BP tools ---
	FString SessionId;

	// Create WBP
	{
		ClaireonWidgetBPTool_Create CreateTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetStringField(TEXT("root_widget_class"), TEXT("CanvasPanel"));

		auto Result = CreateTool.Execute(Args);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to create WBP: %s"), *Result.GetContentAsString()));
			return false;
		}

		// Parse session_id from pretty-printed JSON response
		TSharedPtr<FJsonObject> Json;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Result.GetContentAsString());
		if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
		{
			Json->TryGetStringField(TEXT("session_id"), SessionId);
		}
	}
	if (SessionId.IsEmpty())
	{
		AddError(TEXT("No session_id in create response"));
		return false;
	}

	// Add viewmodel
	{
		ClaireonWidgetBPTool_AddMVVMViewModel AddVMTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		Args->SetStringField(TEXT("viewmodel_name"), TEXT("TreeTestVM"));
		// UMVVMViewModelBase is abstract with no UPROPERTYs and is rejected by
		// add_mvvm_viewmodel's abstract/property-less validation (C7). Use the
		// concrete engine test fixture instead, named by rooted /Script path so
		// resolution is exact.
		Args->SetStringField(TEXT("viewmodel_class"), TEXT("/Script/ModelViewViewModelEditor.MVVMFieldValueChangedTest"));

		auto Result = AddVMTool.Execute(Args);
		TestFalse("add_mvvm_viewmodel should succeed", Result.bIsError);
		if (Result.bIsError)
		{
			AddError(FString::Printf(TEXT("Failed to add viewmodel: %s"), *Result.GetContentAsString()));
			ClaireonWidgetBPTool_Close CloseTool;
			TSharedPtr<FJsonObject> CloseArgs = MakeShared<FJsonObject>();
			CloseArgs->SetStringField(TEXT("session_id"), SessionId);
			CloseTool.Execute(CloseArgs);
			return false;
		}
	}

	// Compile
	{
		ClaireonWidgetBPTool_Compile CompileTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		auto Result = CompileTool.Execute(Args);
		TestFalse("compile should succeed", Result.bIsError);
	}

	// Save
	{
		ClaireonWidgetBPTool_Save SaveTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		auto Result = SaveTool.Execute(Args);
		if (Result.bIsError)
		{
			AddInfo(FString::Printf(TEXT("save returned: %s (non-fatal)"), *Result.GetContentAsString()));
		}
	}

	// Close session
	{
		ClaireonWidgetBPTool_Close CloseTool;
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("session_id"), SessionId);
		CloseTool.Execute(Args);
	}

	// --- Step 2: Call get_widget_tree WITH include_mvvm_bindings=true ---
	ClaireonTool_GetWidgetBPTree TreeTool;
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		Args->SetBoolField(TEXT("include_mvvm_bindings"), true);

		auto Result = TreeTool.Execute(Args);
		TestFalse("get_widget_tree with include_mvvm_bindings=true should succeed", Result.bIsError);

		if (!Result.bIsError)
		{
			// Both flag branches read Data.widget_tree, not the content channel. The old
			// form parsed GetContentAsString() (prose Summary, never JSON) and looked for a
			// TOP-LEVEL "mvvm_viewmodels" -- two independent reasons it could not work:
			// the parse always failed, and SerializeWidgetTree emits mvvm_viewmodels
			// INSIDE the tree object (ClaireonWidgetHelpers.cpp), never at Data top level.
			TestTrue("get_tree returned structured Data", Result.Data.IsValid());

			const TSharedPtr<FJsonObject>* TreeObjPtr = nullptr;
			const bool bHasTree = Result.Data.IsValid()
				&& Result.Data->TryGetObjectField(TEXT("widget_tree"), TreeObjPtr)
				&& TreeObjPtr && (*TreeObjPtr).IsValid();
			TestTrue("Data should contain the 'widget_tree' object", bHasTree);

			if (bHasTree)
			{
				const TArray<TSharedPtr<FJsonValue>>* VMArray = nullptr;
				const bool bHasViewModels = (*TreeObjPtr)->TryGetArrayField(TEXT("mvvm_viewmodels"), VMArray);
				TestTrue("tree should contain 'mvvm_viewmodels' array when the flag is true", bHasViewModels);

				if (bHasViewModels && VMArray)
				{
					TestTrue("mvvm_viewmodels array should have at least 1 entry", VMArray->Num() >= 1);
					AddInfo(FString::Printf(TEXT("mvvm_viewmodels count: %d"), VMArray->Num()));
				}
			}
		}
	}

	// --- Step 3: Call get_widget_tree WITHOUT include_mvvm_bindings ---
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("asset_path"), AssetPath);
		// include_mvvm_bindings defaults to false

		auto Result = TreeTool.Execute(Args);
		TestFalse("get_widget_tree without include_mvvm_bindings should succeed", Result.bIsError);

		if (!Result.bIsError)
		{
			// Same re-point as the flag=true branch. Asserted against Data.widget_tree so
			// the flag can actually influence the outcome; the old top-level content-channel
			// lookup was absent under BOTH flag values, making the TestFalse vacuous.
			TestTrue("get_tree returned structured Data", Result.Data.IsValid());

			const TSharedPtr<FJsonObject>* TreeObjPtr = nullptr;
			const bool bHasTree = Result.Data.IsValid()
				&& Result.Data->TryGetObjectField(TEXT("widget_tree"), TreeObjPtr)
				&& TreeObjPtr && (*TreeObjPtr).IsValid();
			TestTrue("Data should contain the 'widget_tree' object", bHasTree);

			if (bHasTree)
			{
				const TArray<TSharedPtr<FJsonValue>>* VMArray = nullptr;
				const bool bHasViewModels = (*TreeObjPtr)->TryGetArrayField(TEXT("mvvm_viewmodels"), VMArray);
				TestFalse("tree should NOT contain 'mvvm_viewmodels' when the flag is false", bHasViewModels);
				AddInfo(TEXT("Verified mvvm_viewmodels absent when include_mvvm_bindings is false"));
			}
		}
	}

	AddInfo(TEXT("GetWidgetBPTree.MVVMBindingsInTree passed"));
	return true;
}
