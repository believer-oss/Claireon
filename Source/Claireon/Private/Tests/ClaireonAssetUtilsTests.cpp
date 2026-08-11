// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonSafeExec.h"
#include "ClaireonTestTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// ---------------------------------------------------------------------------
// Loading tests
// ---------------------------------------------------------------------------

// Budget: the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs),
// which is not a deliberate perf assertion. This test does a registry query plus a real
// synchronous package load (~4s cold), so the default can never be met. Do not restore it.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Load, NativeGameplayEffect, UNTEST_TIMEOUTMS(30000))
{
	UClass* GEClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffect"));
	UNTEST_ASSERT_PTR(GEClass);
	TArray<FAssetData> Assets = ClaireonAssetUtils::FindAssetsByClass(GEClass, TEXT(""), 1);
	if (Assets.IsEmpty()) co_return; // no GE assets to test with

	FString Error;
	UObject* Obj = ClaireonAssetUtils::LoadAssetForEditing(Assets[0].GetObjectPathString(), Error);
	UNTEST_ASSERT_PTR(Obj);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	UNTEST_EXPECT_TRUE(Obj->IsA(GEClass));
	co_return;
}

// Budget: registry query + real synchronous Blueprint load (~4.5s cold); the 0.50ms
// UNTEST_UNIT default is unreachable and is not a perf assertion. Do not restore it.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Load, BlueprintAbility, UNTEST_TIMEOUTMS(30000))
{
	// Find a GA Blueprint
	UClass* GAClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayAbility"));
	if (!IsValid(GAClass)) co_return;
	TArray<FAssetData> Assets = ClaireonAssetUtils::FindAssetsByClass(GAClass, TEXT(""), 1);
	if (Assets.IsEmpty()) co_return;

	FString Error;
	UObject* Obj = ClaireonAssetUtils::LoadAssetForEditing(Assets[0].GetObjectPathString(), Error);
	// May be a Blueprint CDO or native — either way should not be null
	if (IsValid(Obj))
	{
		UNTEST_EXPECT_TRUE(Error.IsEmpty());
	}
	co_return;
}

// Budget: this attempts a real package resolve for a path that does not exist, so its
// cost is filesystem/asset-registry dependent, not fixed. The bare UNTEST_UNIT default
// is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs) and is not a perf assertion; this
// measured 0.26-0.41ms across four runs and then 0.81ms on a loaded machine. Give it a
// real budget rather than letting host load decide the verdict.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Load, InvalidPath, UNTEST_TIMEOUTMS(10000))
{
	FString Error;
	UObject* Obj = ClaireonAssetUtils::LoadAssetForEditing(TEXT("/Game/DoesNotExist/Fake"), Error);
	UNTEST_EXPECT_NULLPTR(Obj);
	UNTEST_EXPECT_FALSE(Error.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, AssetUtils_Load, EmptyPath, UNTEST_TIMEOUTMS(10000))
{
	FString Error;
	UObject* Obj = ClaireonAssetUtils::LoadAssetForEditing(TEXT(""), Error);
	UNTEST_EXPECT_NULLPTR(Obj);
	UNTEST_EXPECT_FALSE(Error.IsEmpty());
	co_return;
}

// ---------------------------------------------------------------------------
// Search tests
// ---------------------------------------------------------------------------

// Budget: an unlimited FindAssetsByClass is a registry-wide sweep of the whole project
// (~4s). The 0.50ms UNTEST_UNIT default is not a perf assertion. Do not restore it.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Search, FindByClass, UNTEST_TIMEOUTMS(30000))
{
	UClass* GEClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffect"));
	UNTEST_ASSERT_PTR(GEClass);
	TArray<FAssetData> Results = ClaireonAssetUtils::FindAssetsByClass(GEClass);
	UNTEST_EXPECT_TRUE(Results.Num() > 0);
	co_return;
}

// Budget: two registry-wide sweeps (~7.5s). Not a perf assertion. Do not restore 0.50ms.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Search, NameFilter, UNTEST_TIMEOUTMS(30000))
{
	UClass* GEClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffect"));
	UNTEST_ASSERT_PTR(GEClass);
	TArray<FAssetData> All = ClaireonAssetUtils::FindAssetsByClass(GEClass);
	TArray<FAssetData> Filtered = ClaireonAssetUtils::FindAssetsByClass(GEClass, TEXT("Blind"));
	UNTEST_EXPECT_TRUE(Filtered.Num() <= All.Num());
	for (const FAssetData& Asset : Filtered)
	{
		UNTEST_EXPECT_TRUE(Asset.AssetName.ToString().Contains(TEXT("Blind")));
	}
	co_return;
}

// Budget: the limit caps the returned array, not the registry sweep that feeds it (~4s).
// Not a perf assertion. Do not restore 0.50ms.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Search, LimitResults, UNTEST_TIMEOUTMS(30000))
{
	UClass* GEClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffect"));
	UNTEST_ASSERT_PTR(GEClass);
	TArray<FAssetData> Results = ClaireonAssetUtils::FindAssetsByClass(GEClass, TEXT(""), 3);
	UNTEST_EXPECT_TRUE(Results.Num() <= 3);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, AssetUtils_Search, NullClassReturnsEmpty, UNTEST_TIMEOUTMS(10000))
{
	TArray<FAssetData> Results = ClaireonAssetUtils::FindAssetsByClass(nullptr);
	UNTEST_EXPECT_EQ(Results.Num(), 0);
	co_return;
}

// ---------------------------------------------------------------------------
// FindDerivedClasses tests
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, AssetUtils_Derived, FindsSubclasses, UNTEST_TIMEOUTMS(10000))
{
	UClass* GECompClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffectComponent"));
	if (!IsValid(GECompClass)) co_return;
	TArray<UClass*> Results = ClaireonAssetUtils::FindDerivedClasses(GECompClass);
	UNTEST_EXPECT_TRUE(Results.Num() > 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, AssetUtils_Derived, ExcludesAbstractByDefault, UNTEST_TIMEOUTMS(10000))
{
	UClass* GECompClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffectComponent"));
	if (!IsValid(GECompClass)) co_return;
	TArray<UClass*> Results = ClaireonAssetUtils::FindDerivedClasses(GECompClass, false);
	for (UClass* C : Results)
	{
		UNTEST_EXPECT_FALSE(C->HasAnyClassFlags(CLASS_Abstract));
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, AssetUtils_Derived, NameFilterWorks, UNTEST_TIMEOUTMS(10000))
{
	UClass* GECompClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffectComponent"));
	if (!IsValid(GECompClass)) co_return;
	TArray<UClass*> All = ClaireonAssetUtils::FindDerivedClasses(GECompClass);
	TArray<UClass*> Filtered = ClaireonAssetUtils::FindDerivedClasses(GECompClass, false, TEXT("Aura"));
	UNTEST_EXPECT_TRUE(Filtered.Num() <= All.Num());
	for (UClass* C : Filtered)
	{
		UNTEST_EXPECT_TRUE(C->GetName().Contains(TEXT("Aura")));
	}
	co_return;
}

// ---------------------------------------------------------------------------
// AssetDataToJson tests
// ---------------------------------------------------------------------------

// Budget: the AssetDataToJson call itself is trivial, but sourcing the FAssetData needs a
// registry-wide sweep (~4s). Not a perf assertion. Do not restore 0.50ms.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Json, AssetDataToJson, UNTEST_TIMEOUTMS(30000))
{
	UClass* GEClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffect"));
	UNTEST_ASSERT_PTR(GEClass);
	TArray<FAssetData> Assets = ClaireonAssetUtils::FindAssetsByClass(GEClass, TEXT(""), 1);
	if (Assets.IsEmpty()) co_return;

	TSharedPtr<FJsonObject> Json = ClaireonAssetUtils::AssetDataToJson(Assets[0]);
	UNTEST_ASSERT_PTR(Json.Get());
	UNTEST_EXPECT_TRUE(Json->HasField(TEXT("path")));
	UNTEST_EXPECT_TRUE(Json->HasField(TEXT("name")));
	UNTEST_EXPECT_TRUE(Json->HasField(TEXT("class")));
	UNTEST_EXPECT_FALSE(Json->GetStringField(TEXT("path")).IsEmpty());
	UNTEST_EXPECT_FALSE(Json->GetStringField(TEXT("name")).IsEmpty());
	co_return;
}

// ---------------------------------------------------------------------------
// SaveAsset tests (WI-10)
// ---------------------------------------------------------------------------

namespace ClaireonAssetUtilsSaveTestsImpl
{
	// AssetUtilsSaveTests_: file-local discriminator prefix (unity-batch collision safety).

	/** Unique /Game sandbox package path; the saved file is deleted in teardown. */
	static FString AssetUtilsSaveTests_MakeUniquePackagePath(const TCHAR* Tag)
	{
		return FString::Printf(TEXT("/Game/__ClaireonAssetUtilsSaveTests/%s_%s"),
			Tag, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	/** Best-effort teardown for a package created by these tests. */
	static void AssetUtilsSaveTests_Cleanup(UObject* Asset, const FString& PackagePath)
	{
		FString FileName;
		if (FPackageName::TryConvertLongPackageNameToFilename(
				PackagePath, FileName, FPackageName::GetAssetPackageExtension())
			&& IFileManager::Get().FileExists(*FileName))
		{
			IFileManager::Get().Delete(*FileName, /*RequireExists*/ false, /*EvenReadOnly*/ true, /*Quiet*/ true);
		}
		if (IsValid(Asset))
		{
			Asset->ClearFlags(RF_Standalone | RF_Public);
			Asset->MarkAsGarbage();
		}
	}
} // namespace ClaireonAssetUtilsSaveTestsImpl

// A freshly created in-memory package has no disk file yet, so the old
// DoesPackageExist gate rejected it with 'Package file not found'. SaveAsset
// must synthesize the target filename and write the package.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Save, NewInMemoryPackageSavesToDisk, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonAssetUtilsSaveTestsImpl;
	const FString PackagePath = AssetUtilsSaveTests_MakeUniquePackagePath(TEXT("NewPkg"));
	const FString ObjectName = FPackageName::GetShortName(PackagePath);

	UPackage* Package = CreatePackage(*PackagePath);
	UNTEST_ASSERT_PTR(Package);
	UObject* Asset = NewObject<UClaireonSpecDataAsset>(Package, *ObjectName, RF_Public | RF_Standalone);
	UNTEST_ASSERT_PTR(Asset);

	// Precondition of the regression: nothing on disk before the save.
	const bool bOnDiskBefore = FPackageName::DoesPackageExist(PackagePath);

	FString SaveErrorText;
	const bool bSaved = ClaireonAssetUtils::SaveAsset(Asset, SaveErrorText);
	const bool bOnDiskAfter = FPackageName::DoesPackageExist(PackagePath);

	// Teardown before asserting (UNTEST_ASSERT_* co_returns).
	AssetUtilsSaveTests_Cleanup(Asset, PackagePath);

	UNTEST_EXPECT_FALSE(bOnDiskBefore);
	if (!bSaved)
	{
		UNTEST_EXPECT_STREQ(SaveErrorText, TEXT("")); // surface the actual error text in the failure report
	}
	UNTEST_ASSERT_TRUE(bSaved);
	UNTEST_EXPECT_TRUE(SaveErrorText.IsEmpty());
	UNTEST_EXPECT_TRUE(bOnDiskAfter);
	co_return;
}

// A package under no mounted content root has nowhere on disk to go; the
// filename fallback must fail loudly and name the package.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Save, UnmountedPackagePathErrors, UNTEST_TIMEOUTMS(30000))
{
	const FString PackagePath = TEXT("/ClaireonNoSuchMountRoot/SaveTarget");

	UPackage* Package = CreatePackage(*PackagePath);
	UNTEST_ASSERT_PTR(Package);
	UObject* Asset = NewObject<UClaireonSpecDataAsset>(Package, TEXT("SaveTarget"), RF_Public | RF_Standalone);
	UNTEST_ASSERT_PTR(Asset);

	FString SaveErrorText;
	const bool bSaved = ClaireonAssetUtils::SaveAsset(Asset, SaveErrorText);

	Asset->ClearFlags(RF_Standalone | RF_Public);
	Asset->MarkAsGarbage();

	UNTEST_EXPECT_FALSE(bSaved);
	UNTEST_EXPECT_TRUE(SaveErrorText.Contains(TEXT("Cannot resolve a save filename")));
	UNTEST_EXPECT_TRUE(SaveErrorText.Contains(PackagePath));
	co_return;
}

// Pins the exact crash-flag error text that ClaireonDataAssetCreateTests uses
// as its deterministic forced-save-failure seam.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Save, CrashFlagBlocksSaveWithExactError, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonAssetUtilsSaveTestsImpl;
	const FString PackagePath = AssetUtilsSaveTests_MakeUniquePackagePath(TEXT("CrashFlag"));
	const FString ObjectName = FPackageName::GetShortName(PackagePath);

	UPackage* Package = CreatePackage(*PackagePath);
	UNTEST_ASSERT_PTR(Package);
	UObject* Asset = NewObject<UClaireonSpecDataAsset>(Package, *ObjectName, RF_Public | RF_Standalone);
	UNTEST_ASSERT_PTR(Asset);

	ClaireonSafeExec::SetCrashFlag();
	FString SaveErrorText;
	const bool bSaved = ClaireonAssetUtils::SaveAsset(Asset, SaveErrorText);
	ClaireonSafeExec::ClearCrashFlag();

	const bool bOnDisk = FPackageName::DoesPackageExist(PackagePath);
	AssetUtilsSaveTests_Cleanup(Asset, PackagePath);

	UNTEST_EXPECT_FALSE(bSaved);
	UNTEST_EXPECT_STREQ(SaveErrorText,
		TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	UNTEST_EXPECT_FALSE(bOnDisk);
	co_return;
}

// ---------------------------------------------------------------------------
// AssertInnerNameMatchesPackage tests
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, AssetUtils_Assert, AssertInnerNameMatchesPackage_NullAssetFails, UNTEST_TIMEOUTMS(10000))
{
	FString Error;
	const bool bOk = ClaireonAssetUtils::AssertInnerNameMatchesPackage(nullptr, Error);
	UNTEST_EXPECT_FALSE(bOk);
	UNTEST_EXPECT_FALSE(Error.IsEmpty());
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("null")));
	co_return;
}

// Root cause of the historical failure: the fixture instantiated UObject::StaticClass()
// directly. UObject is abstract, so StaticAllocateObject fired a handled ensure
// ("Class which was marked abstract was trying to be loaded in Outer ...") whose callstack
// dump alone took ~23s, blowing the timeout. AssertInnerNameMatchesPackage is
// class-agnostic (it only compares GetName() to the package short name), so any concrete
// UObject works -- use the file's existing concrete fixture class.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Assert, AssertInnerNameMatchesPackage_MatchingNamesPass, UNTEST_TIMEOUTMS(10000))
{
	UPackage* Package = CreatePackage(TEXT("/Engine/Transient/Test_AICNI_Pkg"));
	UNTEST_ASSERT_PTR(Package);
	UObject* Asset = NewObject<UClaireonSpecDataAsset>(Package, TEXT("Test_AICNI_Pkg"), RF_Transient);
	UNTEST_ASSERT_PTR(Asset);

	FString Error;
	const bool bOk = ClaireonAssetUtils::AssertInnerNameMatchesPackage(Asset, Error);
	UNTEST_EXPECT_TRUE(bOk);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, AssetUtils_Assert, AssertInnerNameMatchesPackage_MismatchedNamesFails, UNTEST_TIMEOUTMS(10000))
{
	UPackage* Package = CreatePackage(TEXT("/Engine/Transient/Test_AICNI_Pkg2"));
	UNTEST_ASSERT_PTR(Package);
	// Concrete class, not abstract UObject -- see the MatchingNamesPass note above.
	UObject* Asset = NewObject<UClaireonSpecDataAsset>(Package, TEXT("WrongInnerName"), RF_Transient);
	UNTEST_ASSERT_PTR(Asset);

	FString Error;
	const bool bOk = ClaireonAssetUtils::AssertInnerNameMatchesPackage(Asset, Error);
	UNTEST_EXPECT_FALSE(bOk);
	UNTEST_EXPECT_FALSE(Error.IsEmpty());
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("Test_AICNI_Pkg2")));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("WrongInnerName")));
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("asset_check_inner_name_invariant")));
	co_return;
}

// ---------------------------------------------------------------------------
// EvictInMemoryObject tests
// ---------------------------------------------------------------------------

// Evicting twice under the same asset name used to be FATAL, not merely wrong.
// EvictInMemoryObject renamed into the transient package with a null name, which keeps
// the object's name, so the second eviction renamed on top of the first and
// UObject::Rename called appError ("Renaming an object ... on top of an existing
// object", Obj.cpp). It is reachable from any tool that creates the same asset path
// twice in one session -- bp_create, animbp create/duplicate, widgetbp_create and the
// WidgetBP spec applicator all route through here -- and it took down four of every
// five full Untest sweeps once fixture cleanup stopped deleting in-memory fixtures.
//
// Neither test here can observe the crash directly -- a fatal ends the process, so a
// regression shows up as a crashed run rather than a failed assertion. What they assert
// is the property that prevents it: each eviction frees the name in the source package
// and parks the object under a name distinct from every earlier eviction.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Evict, RepeatedEvictionOfSameNameIsSafe, UNTEST_TIMEOUTMS(10000))
{
	UPackage* Package = CreatePackage(TEXT("/Engine/Transient/Test_Evict_Pkg"));
	UNTEST_ASSERT_PTR(Package);

	const TCHAR* AssetName = TEXT("Test_Evict_Asset");
	TArray<FString> EvictedNames;

	// Three passes, because the failure mode is "second and later", not "second only".
	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		UObject* Occupant = NewObject<UClaireonSpecDataAsset>(Package, AssetName, RF_Public | RF_Standalone);
		UNTEST_ASSERT_PTR(Occupant);

		ClaireonAssetUtils::EvictInMemoryObject(Package, AssetName);

		// Read straight back out; the object is garbage-marked from here on.
		EvictedNames.Add(Occupant->GetName());
		UNTEST_EXPECT_TRUE(Occupant->GetOuter() == GetTransientPackage());

		// The whole point of eviction: the create APIs assert the slot is empty.
		UNTEST_EXPECT_TRUE(StaticFindObject(UObject::StaticClass(), Package, AssetName) == nullptr);
	}

	// No eviction clobbered an earlier one.
	UNTEST_ASSERT_EQ(EvictedNames.Num(), 3);
	UNTEST_EXPECT_FALSE(EvictedNames[0] == EvictedNames[1]);
	UNTEST_EXPECT_FALSE(EvictedNames[1] == EvictedNames[2]);
	UNTEST_EXPECT_FALSE(EvictedNames[0] == EvictedNames[2]);
	co_return;
}

// The Blueprint case, which is the one that actually crashed. UBlueprint::Rename
// forwards to RenameGeneratedClasses, which derives "<Name>_C" and "SKEL_<Name>_C" from
// the new name and calls UClass::Rename on each -- so keeping the Blueprint's name on
// eviction collided on the generated class, not on the Blueprint itself. A unique base
// name is only sufficient because the derived names are checked too.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Evict, RepeatedBlueprintEvictionDoesNotCollideOnGeneratedClass, UNTEST_TIMEOUTMS(60000))
{
	const FString AssetPath = TEXT("/Engine/Transient/Test_Evict_BP");
	const FString AssetName = FPackageName::GetShortName(AssetPath);

	UPackage* Package = CreatePackage(*AssetPath);
	UNTEST_ASSERT_PTR(Package);

	TArray<FString> EvictedClassNames;

	for (int32 Pass = 0; Pass < 3; ++Pass)
	{
		ClaireonAssetUtils::EvictInMemoryObject(Package, AssetName);
		UNTEST_EXPECT_TRUE(StaticFindObject(UObject::StaticClass(), Package, *AssetName) == nullptr);

		UBlueprint* BP = FKismetEditorUtilities::CreateBlueprint(
			AActor::StaticClass(),
			Package,
			FName(*AssetName),
			BPTYPE_Normal,
			UBlueprint::StaticClass(),
			UBlueprintGeneratedClass::StaticClass(),
			NAME_None);
		UNTEST_ASSERT_PTR(BP);

		// The generated class is what collided. Record its name per pass.
		// GeneratedClass is a TSubclassOf, so unwrap it before the pointer assert.
		UClass* GeneratedClass = BP->GeneratedClass.Get();
		UNTEST_ASSERT_PTR(GeneratedClass);
		EvictedClassNames.Add(GeneratedClass->GetName());
	}

	// Every pass got the canonical "<AssetName>_C" back, which is only possible if the
	// prior pass's class really did vacate the package rather than blocking the name.
	UNTEST_ASSERT_EQ(EvictedClassNames.Num(), 3);
	const FString ExpectedClassName = AssetName + TEXT("_C");
	UNTEST_EXPECT_STREQ(*EvictedClassNames[0], *ExpectedClassName);
	UNTEST_EXPECT_STREQ(*EvictedClassNames[1], *ExpectedClassName);
	UNTEST_EXPECT_STREQ(*EvictedClassNames[2], *ExpectedClassName);

	// Leave the slot empty so a later suite in the same process starts clean.
	ClaireonAssetUtils::EvictInMemoryObject(Package, AssetName);
	co_return;
}

// The no-op contract the header promises, kept honest: nothing loaded at that name
// means nothing happens, and a null package is tolerated rather than dereferenced.
UNTEST_UNIT_OPTS(Claireon, AssetUtils_Evict, FreeNameAndNullPackageAreNoOps, UNTEST_TIMEOUTMS(10000))
{
	UPackage* Package = CreatePackage(TEXT("/Engine/Transient/Test_Evict_NoOp_Pkg"));
	UNTEST_ASSERT_PTR(Package);

	ClaireonAssetUtils::EvictInMemoryObject(Package, TEXT("NothingIsLoadedHere"));
	ClaireonAssetUtils::EvictInMemoryObject(nullptr, TEXT("Test_Evict_Asset"));
	ClaireonAssetUtils::EvictInMemoryObject(Package, FString());

	// An unrelated resident object in the same package must survive all three calls.
	UObject* Bystander = NewObject<UClaireonSpecDataAsset>(Package, TEXT("Bystander"), RF_Public);
	UNTEST_ASSERT_PTR(Bystander);
	ClaireonAssetUtils::EvictInMemoryObject(Package, TEXT("NothingIsLoadedHere"));
	UNTEST_EXPECT_TRUE(StaticFindObject(UObject::StaticClass(), Package, TEXT("Bystander")) == Bystander);
	co_return;
}

#endif // WITH_UNTESTED
