// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonTestTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "UObject/Package.h"

// Discover test assets dynamically. Tests that modify assets use undo to restore.

namespace ClaireonPropertyUtilsTestsHelpers
{

UObject* LoadAnyGameplayEffect(FString& OutPath)
{
	UClass* GEClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffect"));
	if (!GEClass) return nullptr;
	TArray<FAssetData> Assets = ClaireonAssetUtils::FindAssetsByClass(GEClass, TEXT(""), 20);
	for (const FAssetData& Asset : Assets)
	{
		FString Error;
		UObject* Obj = ClaireonAssetUtils::LoadAssetForEditing(Asset.GetObjectPathString(), Error);
		if (Obj) { OutPath = Asset.GetObjectPathString(); return Obj; }
	}
	return nullptr;
}

}  // namespace ClaireonPropertyUtilsTestsHelpers

// ---------------------------------------------------------------------------
// Read tests
// ---------------------------------------------------------------------------

UNTEST_UNIT(Claireon, PropertyUtils_Read, Primitive)
{
	FString Path; UObject* GE = ClaireonPropertyUtilsTestsHelpers::LoadAnyGameplayEffect(Path);
	UNTEST_ASSERT_PTR(GE);
	FString Error;
	FString Value = ClaireonPropertyUtils::ReadPropertyByPath(GE, TEXT("DurationPolicy"), Error);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	co_return;
}

UNTEST_UNIT(Claireon, PropertyUtils_Read, Enum)
{
	FString Path; UObject* GE = ClaireonPropertyUtilsTestsHelpers::LoadAnyGameplayEffect(Path);
	UNTEST_ASSERT_PTR(GE);
	FString Error;
	FString Value = ClaireonPropertyUtils::ReadPropertyByPath(GE, TEXT("DurationPolicy"), Error);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	UNTEST_EXPECT_TRUE(Value.Contains(TEXT("Instant")) || Value.Contains(TEXT("Infinite")) || Value.Contains(TEXT("HasDuration")) || Value.IsEmpty());
	co_return;
}

UNTEST_UNIT(Claireon, PropertyUtils_Read, InvalidPath)
{
	FString Path; UObject* GE = ClaireonPropertyUtilsTestsHelpers::LoadAnyGameplayEffect(Path);
	UNTEST_ASSERT_PTR(GE);
	FString Error;
	ClaireonPropertyUtils::ReadPropertyByPath(GE, TEXT("ThisDoesNotExist"), Error);
	UNTEST_EXPECT_FALSE(Error.IsEmpty());
	co_return;
}

UNTEST_UNIT(Claireon, PropertyUtils_Read, NullObject)
{
	FString Error;
	ClaireonPropertyUtils::ReadPropertyByPath(nullptr, TEXT("Anything"), Error);
	UNTEST_EXPECT_FALSE(Error.IsEmpty());
	co_return;
}

UNTEST_UNIT(Claireon, PropertyUtils_Read, ArrayIndexOutOfBounds)
{
	FString Path; UObject* GE = ClaireonPropertyUtilsTestsHelpers::LoadAnyGameplayEffect(Path);
	UNTEST_ASSERT_PTR(GE);
	FString Error;
	ClaireonPropertyUtils::ReadPropertyByPath(GE, TEXT("Modifiers[9999]"), Error);
	// Should error or handle gracefully — no crash
	co_return;
}

UNTEST_UNIT(Claireon, PropertyUtils_Read, ArrayIndexOnModifiers)
{
	UClass* GEClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffect"));
	UNTEST_ASSERT_PTR(GEClass);
	TArray<FAssetData> Assets = ClaireonAssetUtils::FindAssetsByClass(GEClass, TEXT(""), 50);
	UObject* GEWithMods = nullptr;
	for (const FAssetData& Asset : Assets)
	{
		FString Error;
		UObject* Obj = ClaireonAssetUtils::LoadAssetForEditing(Asset.GetObjectPathString(), Error);
		if (!Obj) continue;
		FString Val = ClaireonPropertyUtils::ReadPropertyByPath(Obj, TEXT("Modifiers[0].ModifierOp"), Error);
		if (Error.IsEmpty()) { GEWithMods = Obj; break; }
	}
	if (!GEWithMods) co_return; // skip if none found
	FString Error;
	FString Value = ClaireonPropertyUtils::ReadPropertyByPath(GEWithMods, TEXT("Modifiers[0].ModifierOp"), Error);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	co_return;
}

// ---------------------------------------------------------------------------
// Write tests (with undo)
// ---------------------------------------------------------------------------

UNTEST_UNIT(Claireon, PropertyUtils_Write, PrimitiveAndUndo)
{
	FString Path; UObject* GE = ClaireonPropertyUtilsTestsHelpers::LoadAnyGameplayEffect(Path);
	UNTEST_ASSERT_PTR(GE);
	FString Error;
	FString Original = ClaireonPropertyUtils::ReadPropertyByPath(GE, TEXT("StackLimitCount"), Error);
	UNTEST_ASSERT_TRUE(Error.IsEmpty());
	{
		FScopedTransaction Tx(FText::FromString(TEXT("Test")));
		GE->Modify();
		bool bOk = ClaireonPropertyUtils::WritePropertyByPath(GE, TEXT("StackLimitCount"), TEXT("42"), Error);
		UNTEST_EXPECT_TRUE(bOk);
		FString NewVal = ClaireonPropertyUtils::ReadPropertyByPath(GE, TEXT("StackLimitCount"), Error);
		UNTEST_EXPECT_STREQ(*NewVal, TEXT("42"));
	}
	GEditor->UndoTransaction();
	FString Restored = ClaireonPropertyUtils::ReadPropertyByPath(GE, TEXT("StackLimitCount"), Error);
	UNTEST_EXPECT_STREQ(*Restored, *Original);
	co_return;
}

UNTEST_UNIT(Claireon, PropertyUtils_Write, NullObjectReturnsError)
{
	FString Error;
	bool bOk = ClaireonPropertyUtils::WritePropertyByPath(nullptr, TEXT("X"), TEXT("1"), Error);
	UNTEST_EXPECT_FALSE(bOk);
	UNTEST_EXPECT_FALSE(Error.IsEmpty());
	co_return;
}

// ---------------------------------------------------------------------------
// GetAllProperties tests
// ---------------------------------------------------------------------------

UNTEST_UNIT(Claireon, PropertyUtils_GetAll, ReturnsNonEmpty)
{
	FString Path; UObject* GE = ClaireonPropertyUtilsTestsHelpers::LoadAnyGameplayEffect(Path);
	UNTEST_ASSERT_PTR(GE);
	TSharedPtr<FJsonObject> Props = ClaireonPropertyUtils::GetAllProperties(GE, TEXT(""), 1);
	UNTEST_ASSERT_PTR(Props.Get());
	UNTEST_EXPECT_TRUE(Props->Values.Num() > 0);
	co_return;
}

UNTEST_UNIT(Claireon, PropertyUtils_GetAll, FilterNarrows)
{
	FString Path; UObject* GE = ClaireonPropertyUtilsTestsHelpers::LoadAnyGameplayEffect(Path);
	UNTEST_ASSERT_PTR(GE);
	TSharedPtr<FJsonObject> All = ClaireonPropertyUtils::GetAllProperties(GE, TEXT(""), 0);
	TSharedPtr<FJsonObject> Filtered = ClaireonPropertyUtils::GetAllProperties(GE, TEXT("Duration"), 0);
	UNTEST_EXPECT_TRUE(Filtered->Values.Num() <= All->Values.Num());
	for (auto& Pair : Filtered->Values)
	{
		UNTEST_EXPECT_TRUE(Pair.Key.Contains(TEXT("Duration")));
	}
	co_return;
}

UNTEST_UNIT(Claireon, PropertyUtils_GetAll, NullObjectReturnsEmpty)
{
	TSharedPtr<FJsonObject> Props = ClaireonPropertyUtils::GetAllProperties(nullptr);
	UNTEST_ASSERT_PTR(Props.Get());
	UNTEST_EXPECT_EQ(Props->Values.Num(), 0);
	co_return;
}

// ---------------------------------------------------------------------------
// CreateInstancedArrayElement -- generic guard tests
// ---------------------------------------------------------------------------

UNTEST_UNIT(Claireon, PropertyUtils_Create, RejectsNonInstancedInner)
{
	UClaireonTestNonInstancedHolder* Holder =
		NewObject<UClaireonTestNonInstancedHolder>(GetTransientPackage());
	UNTEST_ASSERT_PTR(Holder);

	FString Err;
	UObject* New = ClaireonPropertyUtils::CreateInstancedArrayElement(
		Holder,
		UObject::StaticClass(),
		TEXT("NonInstancedArray"),
		Err);

	UNTEST_EXPECT_TRUE(New == nullptr);
	UNTEST_EXPECT_FALSE(Err.IsEmpty());
	UNTEST_EXPECT_TRUE(
		Err.Contains(TEXT("Instanced")) ||
		Err.Contains(TEXT("CPF_InstancedReference")));
	UNTEST_EXPECT_TRUE(Holder->NonInstancedArray.Num() == 0);
	co_return;
}

#endif // WITH_UNTESTED
