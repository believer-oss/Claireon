// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonAssetUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

// ---------------------------------------------------------------------------
// Loading tests
// ---------------------------------------------------------------------------

UNTEST_UNIT(Claireon, AssetUtils_Load, NativeGameplayEffect)
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

UNTEST_UNIT(Claireon, AssetUtils_Load, BlueprintAbility)
{
	// Find a GA Blueprint
	UClass* GAClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayAbility"));
	if (!GAClass) co_return;
	TArray<FAssetData> Assets = ClaireonAssetUtils::FindAssetsByClass(GAClass, TEXT(""), 1);
	if (Assets.IsEmpty()) co_return;

	FString Error;
	UObject* Obj = ClaireonAssetUtils::LoadAssetForEditing(Assets[0].GetObjectPathString(), Error);
	// May be a Blueprint CDO or native — either way should not be null
	if (Obj)
	{
		UNTEST_EXPECT_TRUE(Error.IsEmpty());
	}
	co_return;
}

UNTEST_UNIT(Claireon, AssetUtils_Load, InvalidPath)
{
	FString Error;
	UObject* Obj = ClaireonAssetUtils::LoadAssetForEditing(TEXT("/Game/DoesNotExist/Fake"), Error);
	UNTEST_EXPECT_NULLPTR(Obj);
	UNTEST_EXPECT_FALSE(Error.IsEmpty());
	co_return;
}

UNTEST_UNIT(Claireon, AssetUtils_Load, EmptyPath)
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

UNTEST_UNIT(Claireon, AssetUtils_Search, FindByClass)
{
	UClass* GEClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffect"));
	UNTEST_ASSERT_PTR(GEClass);
	TArray<FAssetData> Results = ClaireonAssetUtils::FindAssetsByClass(GEClass);
	UNTEST_EXPECT_TRUE(Results.Num() > 0);
	co_return;
}

UNTEST_UNIT(Claireon, AssetUtils_Search, NameFilter)
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

UNTEST_UNIT(Claireon, AssetUtils_Search, LimitResults)
{
	UClass* GEClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffect"));
	UNTEST_ASSERT_PTR(GEClass);
	TArray<FAssetData> Results = ClaireonAssetUtils::FindAssetsByClass(GEClass, TEXT(""), 3);
	UNTEST_EXPECT_TRUE(Results.Num() <= 3);
	co_return;
}

UNTEST_UNIT(Claireon, AssetUtils_Search, NullClassReturnsEmpty)
{
	TArray<FAssetData> Results = ClaireonAssetUtils::FindAssetsByClass(nullptr);
	UNTEST_EXPECT_EQ(Results.Num(), 0);
	co_return;
}

// ---------------------------------------------------------------------------
// FindDerivedClasses tests
// ---------------------------------------------------------------------------

UNTEST_UNIT(Claireon, AssetUtils_Derived, FindsSubclasses)
{
	UClass* GECompClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffectComponent"));
	if (!GECompClass) co_return;
	TArray<UClass*> Results = ClaireonAssetUtils::FindDerivedClasses(GECompClass);
	UNTEST_EXPECT_TRUE(Results.Num() > 0);
	co_return;
}

UNTEST_UNIT(Claireon, AssetUtils_Derived, ExcludesAbstractByDefault)
{
	UClass* GECompClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffectComponent"));
	if (!GECompClass) co_return;
	TArray<UClass*> Results = ClaireonAssetUtils::FindDerivedClasses(GECompClass, false);
	for (UClass* C : Results)
	{
		UNTEST_EXPECT_FALSE(C->HasAnyClassFlags(CLASS_Abstract));
	}
	co_return;
}

UNTEST_UNIT(Claireon, AssetUtils_Derived, NameFilterWorks)
{
	UClass* GECompClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffectComponent"));
	if (!GECompClass) co_return;
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

UNTEST_UNIT(Claireon, AssetUtils_Json, AssetDataToJson)
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
// AssertInnerNameMatchesPackage tests
// ---------------------------------------------------------------------------

UNTEST_UNIT(Claireon, AssetUtils_Assert, AssertInnerNameMatchesPackage_NullAssetFails)
{
	FString Error;
	const bool bOk = ClaireonAssetUtils::AssertInnerNameMatchesPackage(nullptr, Error);
	UNTEST_EXPECT_FALSE(bOk);
	UNTEST_EXPECT_FALSE(Error.IsEmpty());
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("null")));
	co_return;
}

UNTEST_UNIT(Claireon, AssetUtils_Assert, AssertInnerNameMatchesPackage_MatchingNamesPass)
{
	UPackage* Package = CreatePackage(TEXT("/Engine/Transient/Test_AICNI_Pkg"));
	UNTEST_ASSERT_PTR(Package);
	UObject* Asset = NewObject<UObject>(Package, UObject::StaticClass(), TEXT("Test_AICNI_Pkg"), RF_Transient);
	UNTEST_ASSERT_PTR(Asset);

	FString Error;
	const bool bOk = ClaireonAssetUtils::AssertInnerNameMatchesPackage(Asset, Error);
	UNTEST_EXPECT_TRUE(bOk);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	co_return;
}

UNTEST_UNIT(Claireon, AssetUtils_Assert, AssertInnerNameMatchesPackage_MismatchedNamesFails)
{
	UPackage* Package = CreatePackage(TEXT("/Engine/Transient/Test_AICNI_Pkg2"));
	UNTEST_ASSERT_PTR(Package);
	UObject* Asset = NewObject<UObject>(Package, UObject::StaticClass(), TEXT("WrongInnerName"), RF_Transient);
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

#endif // WITH_UNTESTED
