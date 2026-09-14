// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonTool_EditorLogSearch.h"
#include "Tools/ClaireonLogLineParsing.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ClaireonLog.h"

// Keep UNTEST assertions in the test coroutine; they expand to co_return.
namespace ClaireonTool_EditorLogSearch_spec_Private
{
    // Build a minimal Arguments object with just a pattern field.
    TSharedPtr<FJsonObject> ClaireonToolLogSearchSpec_MakePatternArgs(const FString& Pattern)
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("pattern"), Pattern);
        return Args;
    }

    // Build Arguments with pattern + an exclude_categories array.
    TSharedPtr<FJsonObject> ClaireonToolLogSearchSpec_MakePatternWithExclude(
        const FString& Pattern, const TArray<FString>& ExcludeCategories)
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("pattern"), Pattern);

        TArray<TSharedPtr<FJsonValue>> ExArr;
        for (const FString& C : ExcludeCategories)
        {
            ExArr.Add(MakeShared<FJsonValueString>(C));
        }
        Args->SetArrayField(TEXT("exclude_categories"), ExArr);
        return Args;
    }

    // Returns true when the JSON field exists in Data.
    bool ClaireonToolLogSearchSpec_HasField(const TSharedPtr<FJsonObject>& Data, const FString& Field)
    {
        return Data.IsValid() && Data->HasField(Field);
    }
} // namespace ClaireonTool_EditorLogSearch_spec_Private
using namespace ClaireonTool_EditorLogSearch_spec_Private;

// ===========================================================================
// No-filter path: pattern-only call.
// Asserts the response succeeds and that no category filter fields are present.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, LogSearch, PatternOnlyNoFilterFields, UNTEST_TIMEOUTMS(15000))
{
    ClaireonTool_EditorLogSearch Tool;
    TSharedPtr<FJsonObject> Args = ClaireonToolLogSearchSpec_MakePatternArgs(TEXT("LogClaireon"));
    const IClaireonTool::FToolResult Result = Tool.Execute(Args);

    UNTEST_ASSERT_FALSE(Result.bIsError);
    UNTEST_ASSERT_TRUE(Result.Data.IsValid());

    // No-filter path must NOT produce category filter response fields.
    UNTEST_ASSERT_FALSE(ClaireonToolLogSearchSpec_HasField(Result.Data, TEXT("category_excluded_count")));
    UNTEST_ASSERT_FALSE(Result.Hints.Num() > 0);
    UNTEST_ASSERT_FALSE(ClaireonToolLogSearchSpec_HasField(Result.Data, TEXT("warnings")));
    UNTEST_ASSERT_FALSE(ClaireonToolLogSearchSpec_HasField(Result.Data, TEXT("filtered_lines")));
    UNTEST_ASSERT_FALSE(ClaireonToolLogSearchSpec_HasField(Result.Data, TEXT("include_categories")));
    UNTEST_ASSERT_FALSE(ClaireonToolLogSearchSpec_HasField(Result.Data, TEXT("exclude_categories")));

    // Standard fields must be present.
    UNTEST_ASSERT_TRUE(ClaireonToolLogSearchSpec_HasField(Result.Data, TEXT("total_lines")));
    UNTEST_ASSERT_TRUE(ClaireonToolLogSearchSpec_HasField(Result.Data, TEXT("total_matches")));
    UNTEST_ASSERT_TRUE(ClaireonToolLogSearchSpec_HasField(Result.Data, TEXT("matches")));
    co_return;
}

// ===========================================================================
// Bogus exclude category: call succeeds and warnings array is non-empty.
// The warning must reference log/categories.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, LogSearch, BogusExcludeCategoryProducesWarning, UNTEST_TIMEOUTMS(15000))
{
    ClaireonTool_EditorLogSearch Tool;
    TSharedPtr<FJsonObject> Args = ClaireonToolLogSearchSpec_MakePatternWithExclude(
        TEXT("."), { TEXT("LogDoesNotExist9999") });
    const IClaireonTool::FToolResult Result = Tool.Execute(Args);

    UNTEST_ASSERT_FALSE(Result.bIsError);
    UNTEST_ASSERT_TRUE(Result.Data.IsValid());

    // A warnings array must be present when a bogus category is given.
    UNTEST_ASSERT_TRUE(ClaireonToolLogSearchSpec_HasField(Result.Data, TEXT("warnings")));

    const TArray<TSharedPtr<FJsonValue>>* WarningsArr = nullptr;
    Result.Data->TryGetArrayField(TEXT("warnings"), WarningsArr);
    UNTEST_ASSERT_TRUE(WarningsArr != nullptr && WarningsArr->Num() > 0);

    // The warning must mention log/categories so the user knows where to look.
    const FString WarnText = (*WarningsArr)[0]->AsString();
    UNTEST_ASSERT_TRUE(WarnText.Contains(TEXT("log/categories")));
    co_return;
}

// The live log grows between calls. Compare exclusion counts within one snapshot
// and the stable match prefix across calls, not absolute total-count equality.

UNTEST_UNIT_OPTS(Claireon, LogSearch, BogusExcludeExcludesNothing, UNTEST_TIMEOUTMS(20000))
{
    ClaireonTool_EditorLogSearch Tool;

    // Baseline: pattern-only (no filter).
    const IClaireonTool::FToolResult BaseResult = Tool.Execute(
        ClaireonToolLogSearchSpec_MakePatternArgs(TEXT(".")));
    UNTEST_ASSERT_FALSE(BaseResult.bIsError);
    UNTEST_ASSERT_TRUE(BaseResult.Data.IsValid());

    int32 BaseTotalMatches = 0;
    BaseResult.Data->TryGetNumberField(TEXT("total_matches"), BaseTotalMatches);

    // Filtered: same pattern with a bogus exclude.
    const IClaireonTool::FToolResult FilteredResult = Tool.Execute(
        ClaireonToolLogSearchSpec_MakePatternWithExclude(TEXT("."),
            { TEXT("LogDoesNotExist9999") }));
    UNTEST_ASSERT_FALSE(FilteredResult.bIsError);
    UNTEST_ASSERT_TRUE(FilteredResult.Data.IsValid());

    // (1) Single-snapshot proof that the bogus category excluded nothing.
    int32 CategoryExcludedCount = -1;
    UNTEST_ASSERT_TRUE(FilteredResult.Data->TryGetNumberField(
        TEXT("category_excluded_count"), CategoryExcludedCount));
    UNTEST_ASSERT_EQ(CategoryExcludedCount, 0);

    // (2) Every line in that same snapshot survived the filter.
    int32 FilteredLines = -1;
    int32 FilteredTotalLines = -2;
    UNTEST_ASSERT_TRUE(FilteredResult.Data->TryGetNumberField(TEXT("filtered_lines"), FilteredLines));
    UNTEST_ASSERT_TRUE(FilteredResult.Data->TryGetNumberField(TEXT("total_lines"), FilteredTotalLines));
    UNTEST_ASSERT_EQ(FilteredLines, FilteredTotalLines);

    // (3) The returned match prefix must be identical across both calls. New log
    // lines only ever land at the end of the file, so the earliest matches are
    // stable; any line the filter removed would shift this prefix.
    const TArray<TSharedPtr<FJsonValue>>* BaseMatches = nullptr;
    const TArray<TSharedPtr<FJsonValue>>* FilteredMatches = nullptr;
    UNTEST_ASSERT_TRUE(BaseResult.Data->TryGetArrayField(TEXT("matches"), BaseMatches));
    UNTEST_ASSERT_TRUE(FilteredResult.Data->TryGetArrayField(TEXT("matches"), FilteredMatches));
    UNTEST_ASSERT_TRUE(BaseMatches != nullptr && FilteredMatches != nullptr);
    UNTEST_ASSERT_EQ(FilteredMatches->Num(), BaseMatches->Num());

    for (int32 MatchIdx = 0; MatchIdx < BaseMatches->Num(); ++MatchIdx)
    {
        const TSharedPtr<FJsonObject>* BaseObj = nullptr;
        const TSharedPtr<FJsonObject>* FilteredObj = nullptr;
        UNTEST_ASSERT_TRUE((*BaseMatches)[MatchIdx]->TryGetObject(BaseObj));
        UNTEST_ASSERT_TRUE((*FilteredMatches)[MatchIdx]->TryGetObject(FilteredObj));

        int32 BaseLineNumber = 0;
        int32 FilteredLineNumber = 0;
        (*BaseObj)->TryGetNumberField(TEXT("line_number"), BaseLineNumber);
        (*FilteredObj)->TryGetNumberField(TEXT("line_number"), FilteredLineNumber);
        UNTEST_ASSERT_EQ(FilteredLineNumber, BaseLineNumber);

        FString BaseText;
        FString FilteredText;
        (*BaseObj)->TryGetStringField(TEXT("text"), BaseText);
        (*FilteredObj)->TryGetStringField(TEXT("text"), FilteredText);
        UNTEST_ASSERT_EQ(FilteredText, BaseText);
    }

    // Monotonic backstop: the second read can only have seen MORE lines, never
    // fewer. A drop here would mean the filter really did exclude something.
    int32 FilteredTotalMatches = 0;
    FilteredResult.Data->TryGetNumberField(TEXT("total_matches"), FilteredTotalMatches);
    UNTEST_ASSERT_TRUE(FilteredTotalMatches >= BaseTotalMatches);
    co_return;
}

#endif // WITH_UNTESTED
