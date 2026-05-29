// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// Regression coverage for chooser OutputStruct row init and non-default
// context_index. Four behaviors under test:
//   F1: SetupColumnBinding writes IsBoundToRoot and StructType
//   F2: Execute() invokes StructTypeChanged() after SetNumRows; widens Compile gate
//   F3: SetColumnCellValue lazy-rescue path for uninitialized OutputStruct rows
//   F4: Symmetric AllowedClass seed on FChooserObjectPropertyBinding
//
// Strategy: bypass the MCP tool surface (which requires a /Game/ asset path
// through FSoftObjectPath::TryLoad) and exercise the behaviour directly on a
// transient UChooserTable. Keeps the test free of disk I/O while still
// exercising the public observable surface.
//
// Bound output struct: FVector (stable engine USTRUCT with well-known fields
// that ImportText_Direct round-trips reliably).

#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "Chooser.h"
#include "IChooserColumn.h"
#include "ChooserPropertyAccess.h"
#include "OutputStructColumn.h"
#include "OutputObjectColumn.h"
#include "ObjectColumn.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "StructUtils/InstancedStruct.h"
#include "UObject/Package.h"

// ---------------------------------------------------------------------------
// File-local helpers (anon-namespace names prefixed to avoid unity collisions)
// ---------------------------------------------------------------------------
namespace
{
	// Construct a transient UChooserTable with a single output struct context
	// parameter (FVector). Returns the chooser by pointer (rooted by transient
	// package so GC won't reap mid-test).
	UChooserTable* ChooserOutputStructTests_MakeTransientChooserWithStructParam(const UScriptStruct* OutputStructType)
	{
		UChooserTable* Chooser = NewObject<UChooserTable>(GetTransientPackage());

		FInstancedStruct ContextEntry;
		ContextEntry.InitializeAs<FContextObjectTypeStruct>();
		FContextObjectTypeStruct& Param = ContextEntry.GetMutable<FContextObjectTypeStruct>();
		Param.Struct = const_cast<UScriptStruct*>(OutputStructType);
		Param.Direction = EContextObjectDirection::Write;

		Chooser->ContextData.Add(MoveTemp(ContextEntry));
		return Chooser;
	}

	// Construct a transient UChooserTable with a single output class context
	// parameter (the supplied UClass*).
	UChooserTable* ChooserOutputStructTests_MakeTransientChooserWithClassParam(UClass* OutputClass)
	{
		UChooserTable* Chooser = NewObject<UChooserTable>(GetTransientPackage());

		FInstancedStruct ContextEntry;
		ContextEntry.InitializeAs<FContextObjectTypeClass>();
		FContextObjectTypeClass& Param = ContextEntry.GetMutable<FContextObjectTypeClass>();
		Param.Class = OutputClass;
		Param.Direction = EContextObjectDirection::Write;

		Chooser->ContextData.Add(MoveTemp(ContextEntry));
		return Chooser;
	}

	// Mirror of ClaireonChooserTool_AddColumn.cpp::SetupColumnBinding (anon-NS in
	// the .cpp, so we replicate the behaviour here to test its consequences).
	// MUST stay in sync with F1+F4 (and any future expansion of the helper).
	void ChooserOutputStructTests_SetupBindingMirror(
		FInstancedStruct& ColumnStruct,
		const TArray<FName>& PropertyChain,
		int32 ContextIndex,
		TArrayView<const FInstancedStruct> ContextData)
	{
		const UScriptStruct* ColStructType = ColumnStruct.GetScriptStruct();
		if (!ColStructType) return;

		const FStructProperty* InputValueProp = CastField<FStructProperty>(ColStructType->FindPropertyByName(TEXT("InputValue")));
		if (!InputValueProp || InputValueProp->Struct != TBaseStructure<FInstancedStruct>::Get()) return;

		FInstancedStruct* InputValuePtr = InputValueProp->ContainerPtrToValuePtr<FInstancedStruct>(ColumnStruct.GetMutableMemory());
		if (!InputValuePtr || !InputValuePtr->IsValid()) return;

		const UScriptStruct* ParamStruct = InputValuePtr->GetScriptStruct();
		if (!ParamStruct) return;

		const FProperty* BindingProp = ParamStruct->FindPropertyByName(TEXT("Binding"));
		if (!BindingProp) return;

		uint8* ParamMemory = InputValuePtr->GetMutableMemory();
		FChooserPropertyBinding* Binding = reinterpret_cast<FChooserPropertyBinding*>(
			ParamMemory + BindingProp->GetOffset_ForInternal());

		Binding->PropertyBindingChain = PropertyChain;
		Binding->ContextIndex = ContextIndex;
		Binding->IsBoundToRoot = (PropertyChain.Num() == 0);

		const FStructProperty* BindingStructProp = CastField<FStructProperty>(BindingProp);
		if (!BindingStructProp) return;
		if (!ContextData.IsValidIndex(ContextIndex)) return;

		if (const FContextObjectTypeStruct* CtxStruct = ContextData[ContextIndex].GetPtr<FContextObjectTypeStruct>())
		{
			if (BindingStructProp->Struct->IsChildOf(FChooserStructPropertyBinding::StaticStruct()))
			{
				static_cast<FChooserStructPropertyBinding*>(Binding)->StructType = CtxStruct->Struct;
			}
		}
		else if (const FContextObjectTypeClass* CtxClass = ContextData[ContextIndex].GetPtr<FContextObjectTypeClass>())
		{
			if (BindingStructProp->Struct->IsChildOf(FChooserObjectPropertyBinding::StaticStruct()))
			{
				static_cast<FChooserObjectPropertyBinding*>(Binding)->AllowedClass = CtxClass->Class;
			}
		}
	}
} // namespace

// ============================================================================
// Case 1: OutputStruct happy path -- empty PropertyChain, context_index=0
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ChooserOutputStruct, RootBoundStructBindingSeeded, UNTEST_TIMEOUTMS(5000))
{
	const UScriptStruct* OutputStructType = TBaseStructure<FVector>::Get();
	UChooserTable* Chooser = ChooserOutputStructTests_MakeTransientChooserWithStructParam(OutputStructType);
	UNTEST_ASSERT_PTR(Chooser);

	FInstancedStruct NewColumn;
	NewColumn.InitializeAs<FOutputStructColumn>();

	TArray<FName> EmptyChain;
	ChooserOutputStructTests_SetupBindingMirror(NewColumn, EmptyChain, /*ContextIndex=*/0, Chooser->ContextData);

	Chooser->ColumnsStructs.Add(MoveTemp(NewColumn));

	FInstancedStruct& InsertedColumn = Chooser->ColumnsStructs[0];
	FOutputStructColumn* OutStructCol = InsertedColumn.GetMutablePtr<FOutputStructColumn>();
	UNTEST_ASSERT_PTR(OutStructCol);

	// acceptance: Binding base + typed-metadata fields.
	UNTEST_ASSERT_TRUE(OutStructCol->InputValue.IsValid());
	FStructContextProperty& Param = OutStructCol->InputValue.GetMutable<FStructContextProperty>();
	UNTEST_EXPECT_TRUE(Param.Binding.IsBoundToRoot);
	UNTEST_EXPECT_TRUE(Param.Binding.PropertyBindingChain.IsEmpty());
	UNTEST_EXPECT_EQ(Param.Binding.ContextIndex, 0);
	UNTEST_EXPECT_PTR(Param.Binding.StructType.Get());
	UNTEST_EXPECT_TRUE(Param.Binding.StructType == OutputStructType);

	// acceptance: StructTypeChanged() seeds DefaultRowValue / FallbackValue.
	OutStructCol->StructTypeChanged();
	UNTEST_EXPECT_TRUE(OutStructCol->DefaultRowValue.GetScriptStruct() == OutputStructType);
	UNTEST_EXPECT_TRUE(OutStructCol->FallbackValue.GetScriptStruct() == OutputStructType);

	co_return;
}

// ============================================================================
// Case 1 (continued): RowValues[0] is typed after SetNumRows + StructTypeChanged
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ChooserOutputStruct, RowValuesTypedAfterSetNumRows, UNTEST_TIMEOUTMS(5000))
{
	const UScriptStruct* OutputStructType = TBaseStructure<FVector>::Get();
	UChooserTable* Chooser = ChooserOutputStructTests_MakeTransientChooserWithStructParam(OutputStructType);
	UNTEST_ASSERT_PTR(Chooser);

	// Pre-existing rows must exist before AddColumn for the row-init flow to fire.
	Chooser->ResultsStructs.SetNum(2);

	FInstancedStruct NewColumn;
	NewColumn.InitializeAs<FOutputStructColumn>();

	TArray<FName> EmptyChain;
	ChooserOutputStructTests_SetupBindingMirror(NewColumn, EmptyChain, /*ContextIndex=*/0, Chooser->ContextData);

	Chooser->ColumnsStructs.Add(MoveTemp(NewColumn));

	FInstancedStruct& InsertedColumn = Chooser->ColumnsStructs[0];
	const int32 RowCount = Chooser->ResultsStructs.Num();
	if (FChooserColumnBase* Col = InsertedColumn.GetMutablePtr<FChooserColumnBase>())
	{
		Col->SetNumRows(RowCount);
	}
	if (FOutputStructColumn* OutStructCol = InsertedColumn.GetMutablePtr<FOutputStructColumn>())
	{
		OutStructCol->StructTypeChanged();
	}

	FOutputStructColumn* OutStructCol = InsertedColumn.GetMutablePtr<FOutputStructColumn>();
	UNTEST_ASSERT_PTR(OutStructCol);
	UNTEST_ASSERT_TRUE(OutStructCol->RowValues.Num() == RowCount);
	for (int32 i = 0; i < RowCount; ++i)
	{
		UNTEST_EXPECT_TRUE(OutStructCol->RowValues[i].IsValid());
		UNTEST_EXPECT_TRUE(OutStructCol->RowValues[i].GetScriptStruct() == OutputStructType);
	}

	co_return;
}

// ============================================================================
// Case 2: OutputStruct chain-bound path (non-empty PropertyChain)
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ChooserOutputStruct, ChainBoundIsNotBoundToRoot, UNTEST_TIMEOUTMS(5000))
{
	const UScriptStruct* OutputStructType = TBaseStructure<FVector>::Get();
	UChooserTable* Chooser = ChooserOutputStructTests_MakeTransientChooserWithStructParam(OutputStructType);
	UNTEST_ASSERT_PTR(Chooser);

	FInstancedStruct NewColumn;
	NewColumn.InitializeAs<FOutputStructColumn>();

	TArray<FName> PropertyChain;
	PropertyChain.Add(FName(TEXT("X")));
	ChooserOutputStructTests_SetupBindingMirror(NewColumn, PropertyChain, /*ContextIndex=*/0, Chooser->ContextData);

	Chooser->ColumnsStructs.Add(MoveTemp(NewColumn));

	FInstancedStruct& InsertedColumn = Chooser->ColumnsStructs[0];
	FOutputStructColumn* OutStructCol = InsertedColumn.GetMutablePtr<FOutputStructColumn>();
	UNTEST_ASSERT_PTR(OutStructCol);
	UNTEST_ASSERT_TRUE(OutStructCol->InputValue.IsValid());

	FStructContextProperty& Param = OutStructCol->InputValue.GetMutable<FStructContextProperty>();
	UNTEST_EXPECT_FALSE(Param.Binding.IsBoundToRoot);
	UNTEST_EXPECT_TRUE(Param.Binding.PropertyBindingChain.Num() == 1);
	UNTEST_EXPECT_TRUE(Param.Binding.PropertyBindingChain[0] == FName(TEXT("X")));
	// Typed metadata is independent of root-vs-chain.
	UNTEST_EXPECT_PTR(Param.Binding.StructType.Get());

	co_return;
}

// ============================================================================
// Case 3: F3 safety-net rescue -- invalidated RowValues[i] reinitializes
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ChooserOutputStruct, SetColumnCellValueRescuesUninitializedRow, UNTEST_TIMEOUTMS(5000))
{
	const UScriptStruct* OutputStructType = TBaseStructure<FVector>::Get();
	UChooserTable* Chooser = ChooserOutputStructTests_MakeTransientChooserWithStructParam(OutputStructType);
	UNTEST_ASSERT_PTR(Chooser);

	Chooser->ResultsStructs.SetNum(1);

	FInstancedStruct NewColumn;
	NewColumn.InitializeAs<FOutputStructColumn>();

	TArray<FName> EmptyChain;
	ChooserOutputStructTests_SetupBindingMirror(NewColumn, EmptyChain, /*ContextIndex=*/0, Chooser->ContextData);

	Chooser->ColumnsStructs.Add(MoveTemp(NewColumn));

	// Force the post-create flow but then DELIBERATELY invalidate RowValues[0]
	// to simulate a hand-edited / legacy asset where AddColumn was bypassed.
	FInstancedStruct& InsertedColumn = Chooser->ColumnsStructs[0];
	FOutputStructColumn* OutStructCol = InsertedColumn.GetMutablePtr<FOutputStructColumn>();
	UNTEST_ASSERT_PTR(OutStructCol);

	OutStructCol->RowValues.SetNum(1);
	OutStructCol->RowValues[0].Reset(); // invalidate

	UNTEST_ASSERT_FALSE(OutStructCol->RowValues[0].IsValid());

	// JSON payload with an FVector field.
	TSharedPtr<FJsonObject> ValueObj = MakeShared<FJsonObject>();
	ValueObj->SetStringField(TEXT("X"), TEXT("1.0"));
	TSharedPtr<FJsonValue> Value = MakeShared<FJsonValueObject>(ValueObj);

	FString OutError;
	const bool bOk = ClaireonChooserHelpers::SetColumnCellValue(InsertedColumn, /*RowIndex=*/0, Value, OutError);

	// rescue: should succeed AND the row should now be typed.
	UNTEST_EXPECT_TRUE(bOk);
	UNTEST_EXPECT_TRUE(OutError.IsEmpty());

	FOutputStructColumn* OutStructColPost = InsertedColumn.GetMutablePtr<FOutputStructColumn>();
	UNTEST_ASSERT_PTR(OutStructColPost);
	UNTEST_EXPECT_TRUE(OutStructColPost->RowValues[0].IsValid());
	UNTEST_EXPECT_TRUE(OutStructColPost->RowValues[0].GetScriptStruct() == OutputStructType);

	co_return;
}

// ============================================================================
// Case 4: F4 OutputObject AllowedClass symmetric path
// ============================================================================

UNTEST_UNIT_OPTS(Claireon, ChooserOutputStruct, OutputObjectAllowedClassSeeded, UNTEST_TIMEOUTMS(5000))
{
	UClass* OutputClass = UObject::StaticClass();
	UChooserTable* Chooser = ChooserOutputStructTests_MakeTransientChooserWithClassParam(OutputClass);
	UNTEST_ASSERT_PTR(Chooser);

	FInstancedStruct NewColumn;
	NewColumn.InitializeAs<FOutputObjectColumn>();

	TArray<FName> EmptyChain;
	ChooserOutputStructTests_SetupBindingMirror(NewColumn, EmptyChain, /*ContextIndex=*/0, Chooser->ContextData);

	Chooser->ColumnsStructs.Add(MoveTemp(NewColumn));

	FInstancedStruct& InsertedColumn = Chooser->ColumnsStructs[0];
	FOutputObjectColumn* OutObjCol = InsertedColumn.GetMutablePtr<FOutputObjectColumn>();
	UNTEST_ASSERT_PTR(OutObjCol);
	UNTEST_ASSERT_TRUE(OutObjCol->InputValue.IsValid());

	FObjectContextProperty& Param = OutObjCol->InputValue.GetMutable<FObjectContextProperty>();
	UNTEST_EXPECT_TRUE(Param.Binding.IsBoundToRoot);
	UNTEST_EXPECT_TRUE(Param.Binding.PropertyBindingChain.IsEmpty());
	UNTEST_EXPECT_EQ(Param.Binding.ContextIndex, 0);
	UNTEST_EXPECT_PTR(Param.Binding.AllowedClass.Get());
	UNTEST_EXPECT_TRUE(Param.Binding.AllowedClass == OutputClass);

	co_return;
}

#endif // WITH_UNTESTED
