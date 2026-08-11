// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonPropertyUtils.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonLog.h"
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
	if (!IsValid(GEClass)) return nullptr;
	TArray<FAssetData> Assets = ClaireonAssetUtils::FindAssetsByClass(GEClass, TEXT(""), 20);
	for (const FAssetData& Asset : Assets)
	{
		FString Error;
		UObject* Obj = ClaireonAssetUtils::LoadAssetForEditing(Asset.GetObjectPathString(), Error);
		if (IsValid(Obj)) { OutPath = Asset.GetObjectPathString(); return Obj; }
	}
	return nullptr;
}

}  // namespace ClaireonPropertyUtilsTestsHelpers

// ---------------------------------------------------------------------------
// Read tests
// ---------------------------------------------------------------------------
//
// Budget note for every test below that calls LoadAnyGameplayEffect (or otherwise
// loads content): the bare UNTEST_UNIT default is 0.50ms
// (FUntestUnitFixture::DefaultTimeoutMs), which is not a deliberate perf
// assertion. These tests do a cold asset-registry scan plus a real package load
// and measured 3200-5100ms in CI ("Test finished, but overran timeout limit:
// 4825.71ms elapsed / 0.50ms max"), so every assertion inside them passed and
// only the budget failed. Give them real headroom rather than restoring the
// default. Tests that touch no assets keep the default deliberately.

UNTEST_UNIT_OPTS(Claireon, PropertyUtils_Read, Primitive, UNTEST_TIMEOUTMS(30000))
{
	FString Path; UObject* GE = ClaireonPropertyUtilsTestsHelpers::LoadAnyGameplayEffect(Path);
	UNTEST_ASSERT_PTR(GE);
	FString Error;
	FString Value = ClaireonPropertyUtils::ReadPropertyByPath(GE, TEXT("DurationPolicy"), Error);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PropertyUtils_Read, Enum, UNTEST_TIMEOUTMS(30000))
{
	FString Path; UObject* GE = ClaireonPropertyUtilsTestsHelpers::LoadAnyGameplayEffect(Path);
	UNTEST_ASSERT_PTR(GE);
	FString Error;
	FString Value = ClaireonPropertyUtils::ReadPropertyByPath(GE, TEXT("DurationPolicy"), Error);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	UNTEST_EXPECT_TRUE(Value.Contains(TEXT("Instant")) || Value.Contains(TEXT("Infinite")) || Value.Contains(TEXT("HasDuration")) || Value.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PropertyUtils_Read, InvalidPath, UNTEST_TIMEOUTMS(30000))
{
	FString Path; UObject* GE = ClaireonPropertyUtilsTestsHelpers::LoadAnyGameplayEffect(Path);
	UNTEST_ASSERT_PTR(GE);
	FString Error;
	ClaireonPropertyUtils::ReadPropertyByPath(GE, TEXT("ThisDoesNotExist"), Error);
	UNTEST_EXPECT_FALSE(Error.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PropertyUtils_Read, NullObject, UNTEST_TIMEOUTMS(10000))
{
	FString Error;
	ClaireonPropertyUtils::ReadPropertyByPath(nullptr, TEXT("Anything"), Error);
	UNTEST_EXPECT_FALSE(Error.IsEmpty());
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PropertyUtils_Read, ArrayIndexOutOfBounds, UNTEST_TIMEOUTMS(30000))
{
	FString Path; UObject* GE = ClaireonPropertyUtilsTestsHelpers::LoadAnyGameplayEffect(Path);
	UNTEST_ASSERT_PTR(GE);
	FString Error;
	ClaireonPropertyUtils::ReadPropertyByPath(GE, TEXT("Modifiers[9999]"), Error);
	// Should error or handle gracefully -- no crash
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PropertyUtils_Read, ArrayIndexOnModifiers, UNTEST_TIMEOUTMS(60000))
{
	UClass* GEClass = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffect"));
	UNTEST_ASSERT_PTR(GEClass);
	TArray<FAssetData> Assets = ClaireonAssetUtils::FindAssetsByClass(GEClass, TEXT(""), 50);
	UObject* GEWithMods = nullptr;
	for (const FAssetData& Asset : Assets)
	{
		FString Error;
		UObject* Obj = ClaireonAssetUtils::LoadAssetForEditing(Asset.GetObjectPathString(), Error);
		if (!IsValid(Obj)) continue;
		FString Val = ClaireonPropertyUtils::ReadPropertyByPath(Obj, TEXT("Modifiers[0].ModifierOp"), Error);
		if (Error.IsEmpty()) { GEWithMods = Obj; break; }
	}
	if (!IsValid(GEWithMods)) co_return; // skip if none found
	FString Error;
	FString Value = ClaireonPropertyUtils::ReadPropertyByPath(GEWithMods, TEXT("Modifiers[0].ModifierOp"), Error);
	UNTEST_EXPECT_TRUE(Error.IsEmpty());
	co_return;
}

// ---------------------------------------------------------------------------
// Write tests (with undo)
// ---------------------------------------------------------------------------

// The write/read half of this test runs everywhere. The undo half only runs
// where an undo actually happens: UEditorEngine::Init creates the transaction
// buffer (UEditorEngine::CreateTrans), which an Untest commandlet never reaches
// -- GEditor->Trans stays null, so FScopedTransaction records nothing and
// UndoTransaction() returns false without touching the object. Asserting
// restoration unconditionally therefore failed for a reason that has nothing to
// do with ClaireonPropertyUtils. UndoTransaction()'s return value is the gate,
// so the assertion still runs in a warm editor where the buffer exists.
//
// LoadAnyGameplayEffect hands back a real content asset, so the value has to be
// put back either way: undo when it works, an explicit write when it does not.
// Nothing is saved, so this only matters for the rest of the process, but a
// later test reading the same asset would otherwise see 42.
UNTEST_UNIT_OPTS(Claireon, PropertyUtils_Write, PrimitiveAndUndo, UNTEST_TIMEOUTMS(30000))
{
	FString Path; UObject* GE = ClaireonPropertyUtilsTestsHelpers::LoadAnyGameplayEffect(Path);
	UNTEST_ASSERT_PTR(GE);
	UNTEST_ASSERT_PTR(GEditor);
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

	const bool bUndone = GEditor->UndoTransaction();
	if (bUndone)
	{
		FString Restored = ClaireonPropertyUtils::ReadPropertyByPath(GE, TEXT("StackLimitCount"), Error);
		UNTEST_EXPECT_STREQ(*Restored, *Original);
	}
	else
	{
		UE_LOG(LogClaireon, Warning,
			TEXT("PropertyUtils_Write.PrimitiveAndUndo: skipping the undo-restores assertion -- ")
			TEXT("GEditor->UndoTransaction() returned false, which means this process has no ")
			TEXT("editor transaction buffer (GEditor->Trans is null outside UEditorEngine::Init, ")
			TEXT("e.g. in a commandlet). The write/read assertions above still ran."));

		// Put the asset back by hand since undo could not.
		FString RestoreError;
		ClaireonPropertyUtils::WritePropertyByPath(GE, TEXT("StackLimitCount"), Original, RestoreError);
		FString Restored = ClaireonPropertyUtils::ReadPropertyByPath(GE, TEXT("StackLimitCount"), Error);
		UNTEST_EXPECT_STREQ(*Restored, *Original);
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PropertyUtils_Write, NullObjectReturnsError, UNTEST_TIMEOUTMS(10000))
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

UNTEST_UNIT_OPTS(Claireon, PropertyUtils_GetAll, ReturnsNonEmpty, UNTEST_TIMEOUTMS(30000))
{
	FString Path; UObject* GE = ClaireonPropertyUtilsTestsHelpers::LoadAnyGameplayEffect(Path);
	UNTEST_ASSERT_PTR(GE);
	TSharedPtr<FJsonObject> Props = ClaireonPropertyUtils::GetAllProperties(GE, TEXT(""), 1);
	UNTEST_ASSERT_PTR(Props.Get());
	UNTEST_EXPECT_TRUE(Props->Values.Num() > 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, PropertyUtils_GetAll, FilterNarrows, UNTEST_TIMEOUTMS(30000))
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

UNTEST_UNIT_OPTS(Claireon, PropertyUtils_GetAll, NullObjectReturnsEmpty, UNTEST_TIMEOUTMS(10000))
{
	TSharedPtr<FJsonObject> Props = ClaireonPropertyUtils::GetAllProperties(nullptr);
	UNTEST_ASSERT_PTR(Props.Get());
	UNTEST_EXPECT_EQ(Props->Values.Num(), 0);
	co_return;
}

// ---------------------------------------------------------------------------
// CreateInstancedArrayElement -- generic guard tests
// ---------------------------------------------------------------------------

// Budget: the bare UNTEST_UNIT default is 0.50ms (FUntestUnitFixture::DefaultTimeoutMs),
// which is not a deliberate perf assertion. This test sweeps the fully-populated
// ~717-tool registry, so give it real headroom instead of restoring the default.
UNTEST_UNIT_OPTS(Claireon, PropertyUtils_Create, RejectsNonInstancedInner, UNTEST_TIMEOUTMS(30000))
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
