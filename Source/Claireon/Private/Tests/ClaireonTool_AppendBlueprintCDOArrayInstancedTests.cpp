// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for claireon.append_blueprint_cdo_array_instanced.
//
// Builds throwaway BPs under /Game/__MCPTests/, invokes the tool through its
// JSON Execute entry, asserts both the response payload and the resulting
// FBPVariableDescription / CDO state, and tears the asset back down.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonAssetUtils.h"
#include "Tools/ClaireonTool_AppendBlueprintCDOArrayInstanced.h"
#include "Tools/IClaireonTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ObjectTools.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

#include "ClaireonTestAssetDeletion.h"
namespace ClaireonTool_AppendBlueprintCDOArrayInstancedTests_Private
{
	static const TCHAR* TestBPPath_Happy   = TEXT("/Game/__MCPTests/BP_AppendInstanced_Happy");
	static const TCHAR* TestBPPath_Errors  = TEXT("/Game/__MCPTests/BP_AppendInstanced_Errors");

	// Concrete subobject class for the array's inner type.
	static const TCHAR* CollectorBaseClassPath  = TEXT("/Script/Claireon.ClaireonAppendInstancedBase");
	static const TCHAR* CollectorElementClassPath = TEXT("/Script/Claireon.ClaireonAppendInstancedElement");
	static const TCHAR* CollectorsArrayProperty = TEXT("Collectors");

	// True only when the fixture actually has a .uasset on disk.
	//
	// The fixture below is built in an in-memory package and deliberately NOT
	// saved: nothing in this suite reads the asset back from disk, and
	// append_blueprint_cdo_array_instanced has no save call of its own. Deleting an
	// in-memory fixture buys nothing, and every ObjectTools::ForceDeleteObjects
	// call runs a whole-object-graph referencer scan, which is the trigger for the
	// nondeterministic Niagara-serialization crash documented in
	// Docs/llm/todo/claireon-untest-harness-reliability.md item 1.
	//
	// The check is kept rather than dropping the delete outright because
	// /Game/__MCPTests is deliberately NOT gitignored: a stale .uasset left by an
	// older build (this helper used to call UPackage::Save) or a crashed run must
	// still be cleaned so `git status --porcelain -- Content/` stays empty.
	bool AppendTests_HasFileOnDisk(const FString& AssetOrPackagePath)
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

	void CleanupAppendTestAsset(const FString& AssetPath)
	{
		// In-memory fixture: nothing on disk, nothing to clean, no referencer scan.
		// Freshness between tests sharing a fixture path is guaranteed by the
		// eviction in CreateAppendTestBlueprint, not by this delete.
		if (!AppendTests_HasFileOnDisk(AssetPath))
		{
			return;
		}

		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad(); IsValid(Asset))
		{
			TArray<UObject*> AssetsToDelete;
			AssetsToDelete.Add(Asset);
			ClaireonTestAssetDeletion::DeleteObjectsForTest(AssetsToDelete);
		}
	}

	// Always returns a pristine fixture. Several tests share a fixture path and
	// each mutates the CDO array, so reusing an existing object would make the
	// second test read the first one's appended element as its starting state.
	// Evicting the slot instead of deleting the asset is the same idiom bp_create
	// uses (ClaireonBlueprintHelpers::CreateBlueprint) and, unlike a delete, it
	// runs no referencer scan.
	UBlueprint* CreateAppendTestBlueprint(const FString& AssetPath, UClass* ParentClass)
	{
		if (!IsValid(ParentClass)) return nullptr;

		UPackage* Package = CreatePackage(*AssetPath);
		if (!IsValid(Package)) return nullptr;

		const FString AssetName = FPackageName::GetShortName(AssetPath);
		ClaireonAssetUtils::EvictInMemoryObject(Package, AssetName);

		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);
		if (!IsValid(BP)) return nullptr;

		FAssetRegistryModule::AssetCreated(BP);
		BP->MarkPackageDirty();

		return BP;
	}

	TSharedPtr<FJsonObject> MakeAppendArgs(const TCHAR* AssetPath, const TCHAR* ArrayPath, const TCHAR* ElementClass)
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		if (AssetPath)     Args->SetStringField(TEXT("asset_path"), AssetPath);
		if (ArrayPath)     Args->SetStringField(TEXT("array_property_path"), ArrayPath);
		if (ElementClass)  Args->SetStringField(TEXT("element_class"), ElementClass);
		return Args;
	}

	UClass* ResolveAppendTestClass(const TCHAR* ClassPath)
	{
		return FSoftClassPath(ClassPath).TryLoadClass<UObject>();
	}

	// Read the size of a TArray<UObject*> property by name on the given object.
	int32 ReadAppendArraySize(UObject* Object, FName ArrayName)
	{
		if (!IsValid(Object)) return -1;
		FArrayProperty* ArrProp = CastField<FArrayProperty>(Object->GetClass()->FindPropertyByName(ArrayName));
		if (!ArrProp) return -1;
		FScriptArrayHelper Helper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(Object));
		return Helper.Num();
	}

	// Read the UObject* at Index on a TArray<UObject*> property.
	UObject* ReadAppendArrayElement(UObject* Object, FName ArrayName, int32 Index)
	{
		if (!IsValid(Object)) return nullptr;
		FArrayProperty* ArrProp = CastField<FArrayProperty>(Object->GetClass()->FindPropertyByName(ArrayName));
		if (!ArrProp) return nullptr;
		FObjectProperty* InnerObj = CastField<FObjectProperty>(ArrProp->Inner);
		if (!InnerObj) return nullptr;
		FScriptArrayHelper Helper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(Object));
		if (!Helper.IsValidIndex(Index)) return nullptr;
		return InnerObj->GetObjectPropertyValue(Helper.GetRawPtr(Index));
	}
}
using namespace ClaireonTool_AppendBlueprintCDOArrayInstancedTests_Private;

// ============================================================================
// Test 1: First append returns new_index=0 / new_array_size=1 and the array
// holds a live instance of the requested class.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, AppendBlueprintCDOArrayInstanced, Functional_FirstAppendReturnsIndexZero, UNTEST_TIMEOUTMS(60000))
{
	// ClaireonAppendInstancedBase / ClaireonAppendInstancedElement are declared
	// in-module (Private/Tests/ClaireonTestTypes.h), so they are always available
	// in a process running these tests -- a null here is a defect, not an
	// environment shortfall. The previous `co_return` skipped silently and would
	// have hidden exactly that.
	UClass* BaseClass = ResolveAppendTestClass(CollectorBaseClassPath);
	UClass* ElementClass = ResolveAppendTestClass(CollectorElementClassPath);
	UNTEST_ASSERT_PTR(BaseClass);
	UNTEST_ASSERT_PTR(ElementClass);

	CleanupAppendTestAsset(TestBPPath_Happy);
	UBlueprint* BP = CreateAppendTestBlueprint(TestBPPath_Happy, BaseClass);
	UNTEST_ASSERT_PTR(BP);

	UObject* CDO = IsValid(BP->GeneratedClass) ? BP->GeneratedClass->GetDefaultObject() : nullptr;
	UNTEST_ASSERT_PTR(CDO);
	UNTEST_ASSERT_EQ(ReadAppendArraySize(CDO, FName(CollectorsArrayProperty)), 0);

	ClaireonTool_AppendBlueprintCDOArrayInstanced Tool;
	IClaireonTool::FToolResult R = Tool.Execute(MakeAppendArgs(TestBPPath_Happy, CollectorsArrayProperty, CollectorElementClassPath));
	UNTEST_ASSERT_FALSE(R.bIsError);
	UNTEST_ASSERT_TRUE(R.Data.IsValid());

	int32 NewIndex = -1;
	UNTEST_ASSERT_TRUE(R.Data->TryGetNumberField(TEXT("new_index"), NewIndex));
	UNTEST_EXPECT_EQ(NewIndex, 0);

	int32 NewSize = -1;
	UNTEST_ASSERT_TRUE(R.Data->TryGetNumberField(TEXT("new_array_size"), NewSize));
	UNTEST_EXPECT_EQ(NewSize, 1);

	UObject* Elem0 = ReadAppendArrayElement(CDO, FName(CollectorsArrayProperty), 0);
	UNTEST_ASSERT_PTR(Elem0);
	UNTEST_EXPECT_TRUE(Elem0->IsA(ElementClass));

	CleanupAppendTestAsset(TestBPPath_Happy);
	co_return;
}

// ============================================================================
// Test 2: Second append returns new_index=1, new_array_size=2; the previous
// element is preserved.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, AppendBlueprintCDOArrayInstanced, Functional_SecondAppendReturnsIndexOne, UNTEST_TIMEOUTMS(60000))
{
	UClass* BaseClass = ResolveAppendTestClass(CollectorBaseClassPath);
	UClass* ElementClass = ResolveAppendTestClass(CollectorElementClassPath);
	// In-module fixture classes: always present, so assert instead of skipping.
	UNTEST_ASSERT_PTR(BaseClass);
	UNTEST_ASSERT_PTR(ElementClass);

	CleanupAppendTestAsset(TestBPPath_Happy);
	UBlueprint* BP = CreateAppendTestBlueprint(TestBPPath_Happy, BaseClass);
	UNTEST_ASSERT_PTR(BP);
	UNTEST_ASSERT_PTR(BP->GeneratedClass.Get());

	ClaireonTool_AppendBlueprintCDOArrayInstanced Tool;
	IClaireonTool::FToolResult R1 = Tool.Execute(MakeAppendArgs(TestBPPath_Happy, CollectorsArrayProperty, CollectorElementClassPath));
	UNTEST_ASSERT_FALSE(R1.bIsError);

	IClaireonTool::FToolResult R2 = Tool.Execute(MakeAppendArgs(TestBPPath_Happy, CollectorsArrayProperty, CollectorElementClassPath));
	UNTEST_ASSERT_FALSE(R2.bIsError);
	UNTEST_ASSERT_TRUE(R2.Data.IsValid());

	int32 NewIndex = -1;
	int32 NewSize  = -1;
	UNTEST_ASSERT_TRUE(R2.Data->TryGetNumberField(TEXT("new_index"), NewIndex));
	UNTEST_ASSERT_TRUE(R2.Data->TryGetNumberField(TEXT("new_array_size"), NewSize));
	UNTEST_EXPECT_EQ(NewIndex, 1);
	UNTEST_EXPECT_EQ(NewSize, 2);

	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	UNTEST_ASSERT_PTR(CDO);
	UNTEST_EXPECT_EQ(ReadAppendArraySize(CDO, FName(CollectorsArrayProperty)), 2);
	UObject* Elem0 = ReadAppendArrayElement(CDO, FName(CollectorsArrayProperty), 0);
	UObject* Elem1 = ReadAppendArrayElement(CDO, FName(CollectorsArrayProperty), 1);
	UNTEST_ASSERT_PTR(Elem0);
	UNTEST_ASSERT_PTR(Elem1);
	// Two distinct sub-objects, both of the requested class -- a second append that
	// re-pointed the array at one shared instance would otherwise slip through.
	UNTEST_EXPECT_NE(Elem0, Elem1);
	UNTEST_EXPECT_TRUE(Elem0->IsA(ElementClass));
	UNTEST_EXPECT_TRUE(Elem1->IsA(ElementClass));

	CleanupAppendTestAsset(TestBPPath_Happy);
	co_return;
}

// ============================================================================
// Test 3: Missing required field surfaces a tool error rather than crashing.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, AppendBlueprintCDOArrayInstanced, Errors_MissingAssetPath, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_AppendBlueprintCDOArrayInstanced Tool;
	IClaireonTool::FToolResult R = Tool.Execute(MakeAppendArgs(nullptr, CollectorsArrayProperty, CollectorElementClassPath));
	UNTEST_EXPECT_TRUE(R.bIsError);
	// Pin WHICH error, so any later failure mode cannot pass as this one.
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("asset_path")));
	co_return;
}

// ============================================================================
// Test 4: Passing a non-array property path returns an error with the
// property name surfaced.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, AppendBlueprintCDOArrayInstanced, Errors_NonArrayPath, UNTEST_TIMEOUTMS(60000))
{
	UClass* BaseClass = ResolveAppendTestClass(CollectorBaseClassPath);
	UClass* ElementClass = ResolveAppendTestClass(CollectorElementClassPath);
	// In-module fixture classes: always present, so assert instead of skipping.
	UNTEST_ASSERT_PTR(BaseClass);
	UNTEST_ASSERT_PTR(ElementClass);

	CleanupAppendTestAsset(TestBPPath_Errors);
	UBlueprint* BP = CreateAppendTestBlueprint(TestBPPath_Errors, BaseClass);
	UNTEST_ASSERT_PTR(BP);

	// bIgnoreSelf is a bool on ClaireonAppendInstancedBase, not an array.
	ClaireonTool_AppendBlueprintCDOArrayInstanced Tool;
	IClaireonTool::FToolResult R = Tool.Execute(MakeAppendArgs(TestBPPath_Errors, TEXT("bIgnoreSelf"), CollectorElementClassPath));
	UNTEST_EXPECT_TRUE(R.bIsError);
	// The test header promises "with the property name surfaced" -- pin it, so the
	// assertion cannot be satisfied by an unrelated early-out error.
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("bIgnoreSelf")));
	UNTEST_EXPECT_TRUE(R.ErrorMessage.Contains(TEXT("not a TArray property")));

	CleanupAppendTestAsset(TestBPPath_Errors);
	co_return;
}

// ============================================================================
// Test 5: element_class that is not a subclass of the array's inner type is
// rejected before any mutation.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, AppendBlueprintCDOArrayInstanced, Errors_ElementClassNotSubclass, UNTEST_TIMEOUTMS(60000))
{
	// In-module fixture class: always present, so assert instead of skipping.
	UClass* BaseClass = ResolveAppendTestClass(CollectorBaseClassPath);
	UNTEST_ASSERT_PTR(BaseClass);

	CleanupAppendTestAsset(TestBPPath_Errors);
	UBlueprint* BP = CreateAppendTestBlueprint(TestBPPath_Errors, BaseClass);
	UNTEST_ASSERT_PTR(BP);
	UNTEST_ASSERT_PTR(BP->GeneratedClass.Get());

	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	UNTEST_ASSERT_PTR(CDO);
	const int32 SizeBefore = ReadAppendArraySize(CDO, FName(CollectorsArrayProperty));
	// -1 means the array property was not found at all; the "unchanged on
	// rejection" assertion below must not be satisfied by two -1s.
	UNTEST_ASSERT_EQ(SizeBefore, 0);

	// AActor is not a UClaireonAppendInstancedElementBase subclass.
	ClaireonTool_AppendBlueprintCDOArrayInstanced Tool;
	IClaireonTool::FToolResult R = Tool.Execute(MakeAppendArgs(TestBPPath_Errors, CollectorsArrayProperty, TEXT("/Script/Engine.Actor")));
	UNTEST_EXPECT_TRUE(R.bIsError);

	// Array size must be unchanged on rejection.
	UNTEST_EXPECT_EQ(ReadAppendArraySize(CDO, FName(CollectorsArrayProperty)), SizeBefore);

	CleanupAppendTestAsset(TestBPPath_Errors);
	co_return;
}

#endif // WITH_UNTESTED
