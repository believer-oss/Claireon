// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonChooserHelpers.h"
#include "StructUtils/InstancedStruct.h"
#include "ObjectColumn.h"
#include "Dom/JsonValue.h"
#include "Dom/JsonObject.h"

// ---------------------------------------------------------------------------
// Regression coverage for the FObjectColumn branch of
// ClaireonChooserHelpers::SetColumnCellValue.
//
// Each unit constructs a standalone FObjectColumn via FInstancedStruct (no
// UChooserTable host required) and exercises SetColumnCellValue directly.
// All UNTEST_ASSERT_* macros live in the coroutine body (never inside
// lambdas) -- the macros expand to co_return, which a lambda cannot host.
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, ChooserHelpers, BareStringSetsValueAndDefaultsMatchEqual, UNTEST_TIMEOUTMS(5000))
{
	FInstancedStruct ColumnStruct;
	ColumnStruct.InitializeAs<FObjectColumn>();
	FObjectColumn* ObjCol = ColumnStruct.GetMutablePtr<FObjectColumn>();
	UNTEST_ASSERT_PTR(ObjCol);
	ObjCol->RowValues.SetNum(1);

	TSharedPtr<FJsonValue> Input = MakeShared<FJsonValueString>(TEXT("/Game/VO/Markers/M_Test"));
	FString OutError;
	const bool bOk = ClaireonChooserHelpers::SetColumnCellValue(ColumnStruct, 0, Input, OutError);
	UNTEST_ASSERT_TRUE(bOk);

	const FChooserObjectRowData& Data = ObjCol->RowValues[0];
	UNTEST_ASSERT_STREQ(*Data.Value.ToSoftObjectPath().GetAssetPathString(), TEXT("/Game/VO/Markers/M_Test"));
	UNTEST_ASSERT_TRUE(Data.Comparison == EObjectColumnCellValueComparison::MatchEqual);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ChooserHelpers, ObjectFormMatchEqual, UNTEST_TIMEOUTMS(5000))
{
	FInstancedStruct ColumnStruct;
	ColumnStruct.InitializeAs<FObjectColumn>();
	FObjectColumn* ObjCol = ColumnStruct.GetMutablePtr<FObjectColumn>();
	UNTEST_ASSERT_PTR(ObjCol);
	ObjCol->RowValues.SetNum(1);

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("value"), TEXT("/Game/Path/A"));
	Obj->SetStringField(TEXT("comparison"), TEXT("MatchEqual"));
	TSharedPtr<FJsonValue> Input = MakeShared<FJsonValueObject>(Obj);

	FString OutError;
	const bool bOk = ClaireonChooserHelpers::SetColumnCellValue(ColumnStruct, 0, Input, OutError);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_TRUE(ObjCol->RowValues[0].Comparison == EObjectColumnCellValueComparison::MatchEqual);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ChooserHelpers, ObjectFormMatchNotEqual, UNTEST_TIMEOUTMS(5000))
{
	FInstancedStruct ColumnStruct;
	ColumnStruct.InitializeAs<FObjectColumn>();
	FObjectColumn* ObjCol = ColumnStruct.GetMutablePtr<FObjectColumn>();
	UNTEST_ASSERT_PTR(ObjCol);
	ObjCol->RowValues.SetNum(1);

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("value"), TEXT("/Game/Path/B"));
	Obj->SetStringField(TEXT("comparison"), TEXT("MatchNotEqual"));
	TSharedPtr<FJsonValue> Input = MakeShared<FJsonValueObject>(Obj);

	FString OutError;
	const bool bOk = ClaireonChooserHelpers::SetColumnCellValue(ColumnStruct, 0, Input, OutError);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_TRUE(ObjCol->RowValues[0].Comparison == EObjectColumnCellValueComparison::MatchNotEqual);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ChooserHelpers, ObjectFormMatchAny, UNTEST_TIMEOUTMS(5000))
{
	FInstancedStruct ColumnStruct;
	ColumnStruct.InitializeAs<FObjectColumn>();
	FObjectColumn* ObjCol = ColumnStruct.GetMutablePtr<FObjectColumn>();
	UNTEST_ASSERT_PTR(ObjCol);
	ObjCol->RowValues.SetNum(1);

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("value"), TEXT("/Game/Path/C"));
	Obj->SetStringField(TEXT("comparison"), TEXT("MatchAny"));
	TSharedPtr<FJsonValue> Input = MakeShared<FJsonValueObject>(Obj);

	FString OutError;
	const bool bOk = ClaireonChooserHelpers::SetColumnCellValue(ColumnStruct, 0, Input, OutError);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_TRUE(ObjCol->RowValues[0].Comparison == EObjectColumnCellValueComparison::MatchAny);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ChooserHelpers, EmptyStringClearsValue, UNTEST_TIMEOUTMS(5000))
{
	FInstancedStruct ColumnStruct;
	ColumnStruct.InitializeAs<FObjectColumn>();
	FObjectColumn* ObjCol = ColumnStruct.GetMutablePtr<FObjectColumn>();
	UNTEST_ASSERT_PTR(ObjCol);
	ObjCol->RowValues.SetNum(1);

	TSharedPtr<FJsonValue> Input = MakeShared<FJsonValueString>(TEXT(""));
	FString OutError;
	const bool bOk = ClaireonChooserHelpers::SetColumnCellValue(ColumnStruct, 0, Input, OutError);
	UNTEST_ASSERT_TRUE(bOk);

	const FChooserObjectRowData& Data = ObjCol->RowValues[0];
	UNTEST_ASSERT_TRUE(Data.Value.IsNull());
	UNTEST_ASSERT_TRUE(Data.Comparison == EObjectColumnCellValueComparison::MatchEqual);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ChooserHelpers, InvalidRowIndexErrors, UNTEST_TIMEOUTMS(5000))
{
	FInstancedStruct ColumnStruct;
	ColumnStruct.InitializeAs<FObjectColumn>();
	FObjectColumn* ObjCol = ColumnStruct.GetMutablePtr<FObjectColumn>();
	UNTEST_ASSERT_PTR(ObjCol);
	ObjCol->RowValues.SetNum(1);

	TSharedPtr<FJsonValue> Input = MakeShared<FJsonValueString>(TEXT("/Game/Path/X"));
	FString OutError;
	const bool bOk = ClaireonChooserHelpers::SetColumnCellValue(ColumnStruct, 5, Input, OutError);
	UNTEST_ASSERT_FALSE(bOk);
	UNTEST_ASSERT_TRUE(OutError.Contains(TEXT("out of bounds")));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ChooserHelpers, UnresolvableStringFallsBackToLiteralPath, UNTEST_TIMEOUTMS(5000))
{
	FInstancedStruct ColumnStruct;
	ColumnStruct.InitializeAs<FObjectColumn>();
	FObjectColumn* ObjCol = ColumnStruct.GetMutablePtr<FObjectColumn>();
	UNTEST_ASSERT_PTR(ObjCol);
	ObjCol->RowValues.SetNum(1);

	TSharedPtr<FJsonValue> Input = MakeShared<FJsonValueString>(TEXT("/Game/DoesNotExist/Marker_NoneSuch"));
	FString OutError;
	const bool bOk = ClaireonChooserHelpers::SetColumnCellValue(ColumnStruct, 0, Input, OutError);
	UNTEST_ASSERT_TRUE(bOk);

	const FChooserObjectRowData& Data = ObjCol->RowValues[0];
	UNTEST_ASSERT_STREQ(*Data.Value.ToSoftObjectPath().GetAssetPathString(), TEXT("/Game/DoesNotExist/Marker_NoneSuch"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, ChooserHelpers, ComparisonCaseInsensitive, UNTEST_TIMEOUTMS(5000))
{
	FInstancedStruct ColumnStruct;
	ColumnStruct.InitializeAs<FObjectColumn>();
	FObjectColumn* ObjCol = ColumnStruct.GetMutablePtr<FObjectColumn>();
	UNTEST_ASSERT_PTR(ObjCol);
	ObjCol->RowValues.SetNum(1);

	TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
	Obj->SetStringField(TEXT("value"), TEXT("/Game/P/D"));
	Obj->SetStringField(TEXT("comparison"), TEXT("matchnotequal"));
	TSharedPtr<FJsonValue> Input = MakeShared<FJsonValueObject>(Obj);

	FString OutError;
	const bool bOk = ClaireonChooserHelpers::SetColumnCellValue(ColumnStruct, 0, Input, OutError);
	UNTEST_ASSERT_TRUE(bOk);
	UNTEST_ASSERT_TRUE(ObjCol->RowValues[0].Comparison == EObjectColumnCellValueComparison::MatchNotEqual);
	co_return;
}

#endif // WITH_UNTESTED
