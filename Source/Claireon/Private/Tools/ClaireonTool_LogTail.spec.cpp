// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonTool_LogTail.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ClaireonLog.h"

// ---------------------------------------------------------------------------
// Anonymous-namespace helpers -- all prefixed ClaireonToolLogTailSpec_ to
// avoid anon-namespace unity-build collisions on linux-build-server-v2.
// UNTEST_ASSERT_*/UNTEST_EXPECT_* must never appear inside lambdas (they
// expand to co_return); helpers that need assertions run inline in the test
// coroutine.
// ---------------------------------------------------------------------------
namespace ClaireonTool_LogTail_spec_Private
{
	// Build args for the no-filter (baseline) call.
	TSharedPtr<FJsonObject> ClaireonToolLogTailSpec_MakeBaseArgs()
	{
		return MakeShared<FJsonObject>();
	}

	// Build args with category alias + include_categories (conflict case).
	TSharedPtr<FJsonObject> ClaireonToolLogTailSpec_MakeConflictArgs()
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
		Args->SetStringField(TEXT("category"), TEXT("LogEngine"));

		TArray<TSharedPtr<FJsonValue>> IncArr;
		IncArr.Add(MakeShared<FJsonValueString>(TEXT("LogFSSpawner")));
		Args->SetArrayField(TEXT("include_categories"), IncArr);

		return Args;
	}

	// Build args with a single bogus category in exclude_categories.
	TSharedPtr<FJsonObject> ClaireonToolLogTailSpec_MakeBogusExcludeArgs()
	{
		TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();

		TArray<TSharedPtr<FJsonValue>> ExcArr;
		ExcArr.Add(MakeShared<FJsonValueString>(TEXT("LogClaireonBogusXyz")));
		Args->SetArrayField(TEXT("exclude_categories"), ExcArr);

		return Args;
	}

	// Check whether a FToolResult represents success (non-error).
	// Does not use UNTEST macros -- safe to call from inline test code.
	bool ClaireonToolLogTailSpec_IsSuccess(const IClaireonTool::FToolResult& Result)
	{
		return Result.Data.IsValid() && !Result.bIsError;
	}

	// Check whether the response data contains a "lines" array field.
	bool ClaireonToolLogTailSpec_HasLinesArray(const IClaireonTool::FToolResult& Result)
	{
		if (!Result.Data.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Lines = nullptr;
		return Result.Data->TryGetArrayField(TEXT("lines"), Lines) && Lines != nullptr;
	}

	// Check whether the response data contains a "warnings" array field.
	bool ClaireonToolLogTailSpec_HasWarningsArray(const IClaireonTool::FToolResult& Result)
	{
		if (!Result.Data.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
		return Result.Data->TryGetArrayField(TEXT("warnings"), Warnings) && Warnings != nullptr;
	}

	// Whether the result carries a hint. Reads the structured Result.Hint channel; the former
	// ad-hoc Data.hint string convention is retired, and checking Data here would silently
	// pass forever regardless of what the tool emits.
	bool ClaireonToolLogTailSpec_HasHintField(const IClaireonTool::FToolResult& Result)
	{
		return Result.Hint.IsValid();
	}

	// Get the category_excluded_count field; returns -1 if not present.
	int32 ClaireonToolLogTailSpec_GetExcludedCount(const IClaireonTool::FToolResult& Result)
	{
		if (!Result.Data.IsValid())
		{
			return -1;
		}
		int32 Count = -1;
		Result.Data->TryGetNumberField(TEXT("category_excluded_count"), Count);
		return Count;
	}

	// Check whether a warnings array entry mentions the given substring.
	bool ClaireonToolLogTailSpec_WarningsContain(const IClaireonTool::FToolResult& Result, const FString& Substr)
	{
		if (!Result.Data.IsValid())
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Warnings = nullptr;
		if (!Result.Data->TryGetArrayField(TEXT("warnings"), Warnings) || !Warnings)
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Warnings)
		{
			FString Str;
			if (Entry->TryGetString(Str) && Str.Contains(Substr))
			{
				return true;
			}
		}
		return false;
	}
} // namespace ClaireonTool_LogTail_spec_Private
using namespace ClaireonTool_LogTail_spec_Private;

// ===========================================================================
// No-filter baseline: success, "lines" present, no hint, no warnings.
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, LogTail, NoFilterReturnsLinesShape, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_LogTail Tool;
	TSharedPtr<FJsonObject> Args = ClaireonToolLogTailSpec_MakeBaseArgs();

	const IClaireonTool::FToolResult Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(ClaireonToolLogTailSpec_IsSuccess(Result));
	UNTEST_ASSERT_TRUE(ClaireonToolLogTailSpec_HasLinesArray(Result));
	UNTEST_ASSERT_FALSE(ClaireonToolLogTailSpec_HasHintField(Result));
	UNTEST_ASSERT_FALSE(ClaireonToolLogTailSpec_HasWarningsArray(Result));
	co_return;
}

// ===========================================================================
// category + include_categories conflict: error result mentioning the conflict.
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, LogTail, CategoryAliasConflictReturnsError, UNTEST_TIMEOUTMS(5000))
{
	ClaireonTool_LogTail Tool;
	TSharedPtr<FJsonObject> Args = ClaireonToolLogTailSpec_MakeConflictArgs();

	const IClaireonTool::FToolResult Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(Result.bIsError);
	// The error message should mention the conflict.
	UNTEST_ASSERT_FALSE(Result.ErrorMessage.IsEmpty());
	co_return;
}

// ===========================================================================
// Bogus category in exclude_categories: success with a warnings entry naming it
// and pointing at log/categories.
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, LogTail, BogusExcludeCategoryProducesWarning, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_LogTail Tool;
	TSharedPtr<FJsonObject> Args = ClaireonToolLogTailSpec_MakeBogusExcludeArgs();

	const IClaireonTool::FToolResult Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(ClaireonToolLogTailSpec_IsSuccess(Result));
	UNTEST_ASSERT_TRUE(ClaireonToolLogTailSpec_HasWarningsArray(Result));
	UNTEST_ASSERT_TRUE(ClaireonToolLogTailSpec_WarningsContain(Result, TEXT("LogClaireonBogusXyz")));
	UNTEST_ASSERT_TRUE(ClaireonToolLogTailSpec_WarningsContain(Result, TEXT("log/categories")));
	co_return;
}

// ===========================================================================
// Bogus category in exclude_categories: category_excluded_count == 0, no hint.
// (A filter with a name that matches nothing excludes zero lines.)
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, LogTail, BogusExcludeCategoryZeroExcludedCount, UNTEST_TIMEOUTMS(10000))
{
	ClaireonTool_LogTail Tool;
	TSharedPtr<FJsonObject> Args = ClaireonToolLogTailSpec_MakeBogusExcludeArgs();

	const IClaireonTool::FToolResult Result = Tool.Execute(Args);

	UNTEST_ASSERT_TRUE(ClaireonToolLogTailSpec_IsSuccess(Result));
	UNTEST_ASSERT_EQ(ClaireonToolLogTailSpec_GetExcludedCount(Result), 0);
	UNTEST_ASSERT_FALSE(ClaireonToolLogTailSpec_HasHintField(Result));
	co_return;
}

#endif // WITH_UNTESTED
