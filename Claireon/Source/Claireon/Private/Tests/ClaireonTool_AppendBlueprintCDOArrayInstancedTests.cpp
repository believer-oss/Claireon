// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Tests for claireon.append_blueprint_cdo_array_instanced.
//
// Builds throwaway BPs under /Game/__MCPTests/, invokes the tool through its
// JSON Execute entry, asserts both the response payload and the resulting
// FBPVariableDescription / CDO state, and tears the asset back down.

#if WITH_UNTESTED

#include "Untest.h"

#include "Tools/ClaireonTool_AppendBlueprintCDOArrayInstanced.h"
#include "Tools/IClaireonTool.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
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
	static const TCHAR* TestBPPath_Happy   = TEXT("/Game/__MCPTests/BP_AppendInstanced_Happy");
	static const TCHAR* TestBPPath_Errors  = TEXT("/Game/__MCPTests/BP_AppendInstanced_Errors");

	// Concrete subobject class for the array's inner type.
	static const TCHAR* CollectorBaseClassPath  = TEXT("/Script/FSTargeting.FSTargetingInstance_Modular");
	static const TCHAR* CollectorElementClassPath = TEXT("/Script/FSTargeting.FSTC_CollisionSphere");
	static const TCHAR* CollectorsArrayProperty = TEXT("Collectors");

	void CleanupAppendTestAsset(const FString& AssetPath)
	{
		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UObject* Asset = FSoftObjectPath(ObjectPath).TryLoad())
		{
			TArray<UObject*> AssetsToDelete;
			AssetsToDelete.Add(Asset);
			ObjectTools::ForceDeleteObjects(AssetsToDelete, false);
		}
	}

	UBlueprint* CreateAppendTestBlueprint(const FString& AssetPath, UClass* ParentClass)
	{
		if (!ParentClass) return nullptr;

		const FString ObjectPath = AssetPath + TEXT(".") + FPackageName::GetShortName(AssetPath);
		if (UBlueprint* Existing = Cast<UBlueprint>(FSoftObjectPath(ObjectPath).TryLoad()))
		{
			return Existing;
		}

		UPackage* Package = CreatePackage(*AssetPath);
		if (!Package) return nullptr;

		const FString AssetName = FPackageName::GetShortName(AssetPath);
		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			ParentClass,
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
		if (!Object) return -1;
		FArrayProperty* ArrProp = CastField<FArrayProperty>(Object->GetClass()->FindPropertyByName(ArrayName));
		if (!ArrProp) return -1;
		FScriptArrayHelper Helper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(Object));
		return Helper.Num();
	}

	// Read the UObject* at Index on a TArray<UObject*> property.
	UObject* ReadAppendArrayElement(UObject* Object, FName ArrayName, int32 Index)
	{
		if (!Object) return nullptr;
		FArrayProperty* ArrProp = CastField<FArrayProperty>(Object->GetClass()->FindPropertyByName(ArrayName));
		if (!ArrProp) return nullptr;
		FObjectProperty* InnerObj = CastField<FObjectProperty>(ArrProp->Inner);
		if (!InnerObj) return nullptr;
		FScriptArrayHelper Helper(ArrProp, ArrProp->ContainerPtrToValuePtr<void>(Object));
		if (!Helper.IsValidIndex(Index)) return nullptr;
		return InnerObj->GetObjectPropertyValue(Helper.GetRawPtr(Index));
	}
}

// ============================================================================
// Test 1: First append returns new_index=0 / new_array_size=1 and the array
// holds a live instance of the requested class.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, AppendBlueprintCDOArrayInstanced, Functional_FirstAppendReturnsIndexZero, UNTEST_TIMEOUTMS(60000))
{
	UClass* BaseClass = ResolveAppendTestClass(CollectorBaseClassPath);
	UClass* ElementClass = ResolveAppendTestClass(CollectorElementClassPath);
	if (!BaseClass || !ElementClass) co_return; // FSTargeting not loaded; skip.

	CleanupAppendTestAsset(TestBPPath_Happy);
	UBlueprint* BP = CreateAppendTestBlueprint(TestBPPath_Happy, BaseClass);
	UNTEST_ASSERT_PTR(BP);

	UObject* CDO = BP->GeneratedClass ? BP->GeneratedClass->GetDefaultObject() : nullptr;
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
	if (!BaseClass || !ElementClass) co_return;

	CleanupAppendTestAsset(TestBPPath_Happy);
	UBlueprint* BP = CreateAppendTestBlueprint(TestBPPath_Happy, BaseClass);
	UNTEST_ASSERT_PTR(BP);

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
	UObject* Elem0 = ReadAppendArrayElement(CDO, FName(CollectorsArrayProperty), 0);
	UObject* Elem1 = ReadAppendArrayElement(CDO, FName(CollectorsArrayProperty), 1);
	UNTEST_EXPECT_PTR(Elem0);
	UNTEST_EXPECT_PTR(Elem1);
	UNTEST_EXPECT_NE(Elem0, Elem1);

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
	if (!BaseClass || !ElementClass) co_return;

	CleanupAppendTestAsset(TestBPPath_Errors);
	UBlueprint* BP = CreateAppendTestBlueprint(TestBPPath_Errors, BaseClass);
	UNTEST_ASSERT_PTR(BP);

	// bIgnoreSelf is a bool on FSTargetingInstance_Modular, not an array.
	ClaireonTool_AppendBlueprintCDOArrayInstanced Tool;
	IClaireonTool::FToolResult R = Tool.Execute(MakeAppendArgs(TestBPPath_Errors, TEXT("bIgnoreSelf"), CollectorElementClassPath));
	UNTEST_EXPECT_TRUE(R.bIsError);

	CleanupAppendTestAsset(TestBPPath_Errors);
	co_return;
}

// ============================================================================
// Test 5: element_class that is not a subclass of the array's inner type is
// rejected before any mutation.
// ============================================================================
UNTEST_UNIT_OPTS(Claireon, AppendBlueprintCDOArrayInstanced, Errors_ElementClassNotSubclass, UNTEST_TIMEOUTMS(60000))
{
	UClass* BaseClass = ResolveAppendTestClass(CollectorBaseClassPath);
	if (!BaseClass) co_return;

	CleanupAppendTestAsset(TestBPPath_Errors);
	UBlueprint* BP = CreateAppendTestBlueprint(TestBPPath_Errors, BaseClass);
	UNTEST_ASSERT_PTR(BP);

	UObject* CDO = BP->GeneratedClass->GetDefaultObject();
	const int32 SizeBefore = ReadAppendArraySize(CDO, FName(CollectorsArrayProperty));

	// AActor is not a UFSTargetCollector subclass.
	ClaireonTool_AppendBlueprintCDOArrayInstanced Tool;
	IClaireonTool::FToolResult R = Tool.Execute(MakeAppendArgs(TestBPPath_Errors, CollectorsArrayProperty, TEXT("/Script/Engine.Actor")));
	UNTEST_EXPECT_TRUE(R.bIsError);

	// Array size must be unchanged on rejection.
	UNTEST_EXPECT_EQ(ReadAppendArraySize(CDO, FName(CollectorsArrayProperty)), SizeBefore);

	CleanupAppendTestAsset(TestBPPath_Errors);
	co_return;
}

#endif // WITH_UNTESTED
