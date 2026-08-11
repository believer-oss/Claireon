// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#pragma once

// ============================================================================
// READ THIS BEFORE ASSERTING ANYTHING ABOUT AN FToolResult.
//
// THE CONVENTION
// --------------
//   * Assert STRUCTURED FACTS against `Result.Data`, unconditionally.
//     Row counts, ids, node guids, booleans, arrays, nested objects: all of it
//     lives in `Data`. Use the UNTEST_CLAIREON_DATA_* macros below.
//   * Reserve `Result.Summary` / `Result.GetContentAsString()` for asserting
//     HUMAN-READABLE TEXT SHAPE -- "the error names the parameter", "the CSV
//     header lists every column", "the warning names the remedy".
//
// WHY THIS IS A TRAP (the single ambiguity that killed ~170 assertions)
// --------------------------------------------------------------------
// `GetContentAsString()` is exactly `bIsError ? ErrorMessage : Summary`. It
// NEVER reads `Data`. And the tool families disagree about what goes in
// `Summary`, with no naming signal to tell them apart:
//
//   * WHOLE PAYLOAD IN Summary (Data is often null):
//       - `BuildStateResponse` on the blueprint / widget / anim edit bases
//       - the log tools, niagara-compile, datatable export_json / export_csv
//     For these, `GetContentAsString().Contains(...)` genuinely works, which is
//     what makes the wrong habit look correct.
//
//   * ONE-LINE COUNT IN Summary, EVERYTHING IN Data:
//       - the inspect tools, and most decomposed per-operation tools
//     For these, `GetContentAsString().Contains("some_id")` can NEVER match, so
//     the assertion is vacuous -- it passes on an empty result, and it passes on
//     a result that lost the field entirely.
//
// So: `Data` is the contract. `Summary` is prose. Never probe `Summary` for a
// value you could read out of `Data`.
//
// HOW TO USE THE MACROS
// ---------------------
//   FString GraphName;
//   UNTEST_CLAIREON_DATA_STR(R, "graph_name", GraphName);   // 1 line
//
// versus the shape it replaces, which is longer AND reports nothing useful when
// it trips ("Assert failed: R.Data->TryGetStringField(...)false == true"):
//
//   FString GraphName;
//   UNTEST_ASSERT_TRUE(R.Data.IsValid());
//   UNTEST_ASSERT_TRUE(R.Data->TryGetStringField(TEXT("graph_name"), GraphName));
//
// On failure the macros record an error that names the field, says which of the
// three things went wrong -- the tool errored / there is no `Data` at all /
// the key is absent or the wrong JSON type -- and dumps the keys `Data`
// actually carried. "You asserted against Summary but the payload is in Data"
// is then readable straight off the failure line.
//
// CONSTRAINTS (both are load-bearing, do not work around them)
// ------------------------------------------------------------
//   * These macros expand to `co_return` (they are ASSERT-flavoured: the test
//     stops on the first miss, because the next line would read an
//     uninitialised out-param). Like every UNTEST_* macro they therefore CANNOT
//     appear inside a lambda body. Assert at test scope.
//   * `Field` must be a bare string literal, not an FString: it is passed
//     through `TEXT()`.
// ============================================================================

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonTypes.h"
#include "Tools/IClaireonTool.h"

// File-local named namespace: anonymous namespaces from separate .cpp files get
// merged into one TU under linux-build-server-v2 unity batching and collide.
namespace ClaireonTestDataAssertions
{
	inline const TCHAR* JsonTypeName(EJson Type)
	{
		switch (Type)
		{
			case EJson::None:    return TEXT("none");
			case EJson::Null:    return TEXT("null");
			case EJson::String:  return TEXT("string");
			case EJson::Number:  return TEXT("number");
			case EJson::Boolean: return TEXT("boolean");
			case EJson::Array:   return TEXT("array");
			case EJson::Object:  return TEXT("object");
			default:             return TEXT("unknown");
		}
	}

	/** Comma-separated "key(type)" list of everything Data actually carries. */
	inline FString DescribeAvailableKeys(const TSharedPtr<FJsonObject>& Data)
	{
		if (!Data.IsValid())
		{
			return TEXT("<no Data object>");
		}
		if (Data->Values.Num() == 0)
		{
			return TEXT("<Data object is empty>");
		}
		TArray<FString> Parts;
		Parts.Reserve(Data->Values.Num());
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Data->Values)
		{
			const TCHAR* TypeName = Pair.Value.IsValid()
				? JsonTypeName(Pair.Value->Type)
				: TEXT("invalid");
			Parts.Add(FString::Printf(TEXT("%s(%s)"), *Pair.Key, TypeName));
		}
		Parts.Sort();
		return FString::Join(Parts, TEXT(", "));
	}

	/** First 200 chars of Summary, so a Summary-carrying family is obvious. */
	inline FString DescribeSummaryHead(const FString& Summary)
	{
		if (Summary.IsEmpty())
		{
			return TEXT("<empty>");
		}
		return Summary.Len() <= 200 ? Summary : (Summary.Left(200) + TEXT("..."));
	}

	/**
	 * Build the failure text for a Data probe that came up empty. Names the
	 * field, distinguishes the three causes, and dumps the available keys plus
	 * the head of Summary so "the payload is in Summary, not Data" reads
	 * directly off the failure.
	 */
	inline FString MakeDataFieldDiag(
		const IClaireonTool::FToolResult& Result,
		const TCHAR* Field,
		const TCHAR* ExpectedKind)
	{
		if (Result.bIsError)
		{
			return FString::Printf(
				TEXT("Result.Data[\"%s\"] (%s) unavailable: the tool returned an ERROR. ")
				TEXT("ErrorMessage='%s'. Assert the error path instead, or fix the call."),
				Field, ExpectedKind, *Result.ErrorMessage);
		}
		if (!Result.Data.IsValid())
		{
			return FString::Printf(
				TEXT("Result.Data[\"%s\"] (%s) unavailable: the tool returned NO Data object at all. ")
				TEXT("This family puts its payload in Summary -- assert text shape against ")
				TEXT("GetContentAsString() instead. Summary='%s'"),
				Field, ExpectedKind, *DescribeSummaryHead(Result.Summary));
		}
		return FString::Printf(
			TEXT("Result.Data[\"%s\"] is missing or not a %s. Data carries: %s. ")
			TEXT("(If the value you want is only in the prose, this tool is a ")
			TEXT("Summary-carrying family -- see ClaireonTestDataAssertions.h.) Summary='%s'"),
			Field, ExpectedKind, *DescribeAvailableKeys(Result.Data),
			*DescribeSummaryHead(Result.Summary));
	}

	inline bool FetchString(const IClaireonTool::FToolResult& Result, const TCHAR* Field, FString& Out)
	{
		return !Result.bIsError && Result.Data.IsValid() && Result.Data->TryGetStringField(Field, Out);
	}

	inline bool FetchInt(const IClaireonTool::FToolResult& Result, const TCHAR* Field, int32& Out)
	{
		return !Result.bIsError && Result.Data.IsValid() && Result.Data->TryGetNumberField(Field, Out);
	}

	inline bool FetchDouble(const IClaireonTool::FToolResult& Result, const TCHAR* Field, double& Out)
	{
		return !Result.bIsError && Result.Data.IsValid() && Result.Data->TryGetNumberField(Field, Out);
	}

	inline bool FetchBool(const IClaireonTool::FToolResult& Result, const TCHAR* Field, bool& Out)
	{
		return !Result.bIsError && Result.Data.IsValid() && Result.Data->TryGetBoolField(Field, Out);
	}

	inline bool FetchArray(
		const IClaireonTool::FToolResult& Result,
		const TCHAR* Field,
		const TArray<TSharedPtr<FJsonValue>>*& Out)
	{
		return !Result.bIsError && Result.Data.IsValid()
			&& Result.Data->TryGetArrayField(Field, Out) && Out != nullptr;
	}

	inline bool FetchObject(
		const IClaireonTool::FToolResult& Result,
		const TCHAR* Field,
		const TSharedPtr<FJsonObject>*& Out)
	{
		return !Result.bIsError && Result.Data.IsValid()
			&& Result.Data->TryGetObjectField(Field, Out) && Out != nullptr && (*Out).IsValid();
	}
} // namespace ClaireonTestDataAssertions

// ----------------------------------------------------------------------------
// Assertion macros. Test-scope only (they expand to co_return; see the header
// comment). `Field` must be a bare string literal; `OutVar` must already be
// declared, so the value stays usable after the macro.
// ----------------------------------------------------------------------------
#define UNTEST_CLAIREON_DATA_IMPL(Fetcher, Result, Field, OutVar, ExpectedKind)          \
	do                                                                                   \
	{                                                                                    \
		if (!ClaireonTestDataAssertions::Fetcher((Result), TEXT(Field), (OutVar)))        \
		{                                                                                \
			TestContext.AddError(FString::Printf(TEXT("(%s:%d) Assert failed: %s"),       \
				TEXT(__FILE__), __LINE__,                                                \
				*ClaireonTestDataAssertions::MakeDataFieldDiag(                           \
					(Result), TEXT(Field), TEXT(ExpectedKind))));                         \
			co_return;                                                                   \
		}                                                                                \
	} while (0)

/** FString OutVar must be declared first. */
#define UNTEST_CLAIREON_DATA_STR(Result, Field, OutVar) \
	UNTEST_CLAIREON_DATA_IMPL(FetchString, Result, Field, OutVar, "string")

/** int32 OutVar must be declared first. */
#define UNTEST_CLAIREON_DATA_INT(Result, Field, OutVar) \
	UNTEST_CLAIREON_DATA_IMPL(FetchInt, Result, Field, OutVar, "number")

/** double OutVar must be declared first. */
#define UNTEST_CLAIREON_DATA_DOUBLE(Result, Field, OutVar) \
	UNTEST_CLAIREON_DATA_IMPL(FetchDouble, Result, Field, OutVar, "number")

/** bool OutVar must be declared first. */
#define UNTEST_CLAIREON_DATA_BOOL(Result, Field, OutVar) \
	UNTEST_CLAIREON_DATA_IMPL(FetchBool, Result, Field, OutVar, "boolean")

/** `const TArray<TSharedPtr<FJsonValue>>* OutVar = nullptr;` must be declared first. */
#define UNTEST_CLAIREON_DATA_ARRAY(Result, Field, OutVar) \
	UNTEST_CLAIREON_DATA_IMPL(FetchArray, Result, Field, OutVar, "array")

/** `const TSharedPtr<FJsonObject>* OutVar = nullptr;` must be declared first. */
#define UNTEST_CLAIREON_DATA_OBJECT(Result, Field, OutVar) \
	UNTEST_CLAIREON_DATA_IMPL(FetchObject, Result, Field, OutVar, "object")
