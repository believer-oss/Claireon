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

	// SetColumnCellValue runs the input through ClaireonPathResolver::Resolve,
	// which canonicalizes a bare package path into object-path form by
	// appending ".<AssetName>". So the stored soft path is always
	// "/Game/VO/Markers/M_Test.M_Test", never the bare package path the old
	// assertion expected -- that assertion could not hold for any input of
	// this shape.
	const FChooserObjectRowData& Data = ObjCol->RowValues[0];
	UNTEST_ASSERT_STREQ(*Data.Value.ToSoftObjectPath().GetAssetPathString(), TEXT("/Game/VO/Markers/M_Test.M_Test"));
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

// Renamed from UnresolvableStringFallsBackToLiteralPath: there is no
// literal-path fallback any more (SetColumnCellValue used to have one, it was
// unreachable dead code, and it has been removed), and the path this test feeds
// in is not unresolvable -- it merely names an asset that does not exist. The
// name now says what the test asserts.
UNTEST_UNIT_OPTS(Claireon, ChooserHelpers, NonExistentAssetPathStoredCanonicalized, UNTEST_TIMEOUTMS(5000))
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

	// A /Game/ path that names no existing asset is still accepted: Resolve()
	// is purely syntactic for /Game/ paths (it performs no asset-registry
	// existence check), so it succeeds and canonicalizes to object-path form.
	// The old assertion expected the bare package path, i.e. it assumed
	// Resolve() would FAIL here and SetColumnCellValue would take its
	// FSoftObjectPath(AssetPath) literal-fallback branch. That branch was
	// unreachable for any well-formed /Game/ path, so the assertion could
	// never hold; the branch itself is now gone.
	const FChooserObjectRowData& Data = ObjCol->RowValues[0];
	UNTEST_ASSERT_STREQ(*Data.Value.ToSoftObjectPath().GetAssetPathString(),
		TEXT("/Game/DoesNotExist/Marker_NoneSuch.Marker_NoneSuch"));
	co_return;
}

// Companion to the test above: an input the resolver genuinely cannot resolve
// must produce an error, not a silently stored garbage FSoftObjectPath. An
// extension-only string is one of the few inputs that makes Resolve() fail
// ("Path contained only a file extension."), and it is exactly the kind of value
// the removed literal fallback used to swallow.
UNTEST_UNIT_OPTS(Claireon, ChooserHelpers, UnresolvableObjectPathErrors, UNTEST_TIMEOUTMS(5000))
{
	FInstancedStruct ColumnStruct;
	ColumnStruct.InitializeAs<FObjectColumn>();
	FObjectColumn* ObjCol = ColumnStruct.GetMutablePtr<FObjectColumn>();
	UNTEST_ASSERT_PTR(ObjCol);
	ObjCol->RowValues.SetNum(1);

	TSharedPtr<FJsonValue> Input = MakeShared<FJsonValueString>(TEXT(".uasset"));
	FString OutError;
	const bool bOk = ClaireonChooserHelpers::SetColumnCellValue(ColumnStruct, 0, Input, OutError);
	UNTEST_ASSERT_FALSE(bOk);
	UNTEST_ASSERT_TRUE(OutError.Contains(TEXT("could not be resolved")));
	// Nothing was written into the row.
	UNTEST_ASSERT_TRUE(ObjCol->RowValues[0].Value.IsNull());
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
