// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

// ===========================================================================
// Spec tests for ClaireonTool_LogCategories (log/categories tool).
//
// v2/Linux hygiene:
//   - anonymous-namespace helpers prefixed ClaireonToolLogCategoriesSpec_
//   - UNTEST_ASSERT_*/UNTEST_EXPECT_* never inside lambdas
//   - unique .spec.cpp basename (ClaireonTool_LogCategories.spec.cpp)
// ===========================================================================

#include "Untest.h"
#include "Tools/ClaireonTool_LogCategories.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "ClaireonLog.h"

namespace ClaireonTool_LogCategories_spec_Private
{
    // Build an empty arguments object (no contains filter).
    TSharedPtr<FJsonObject> ClaireonToolLogCategoriesSpec_MakeEmptyArgs()
    {
        return MakeShared<FJsonObject>();
    }

    // Build an arguments object with a "contains" string filter.
    TSharedPtr<FJsonObject> ClaireonToolLogCategoriesSpec_MakeContainsArgs(const FString& Contains)
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        Args->SetStringField(TEXT("contains"), Contains);
        return Args;
    }

    // Return the categories array from a successful tool result, or null.
    const TArray<TSharedPtr<FJsonValue>>* ClaireonToolLogCategoriesSpec_GetCategories(
        const IClaireonTool::FToolResult& Result)
    {
        if (!Result.Data.IsValid())
        {
            return nullptr;
        }
        const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
        Result.Data->TryGetArrayField(TEXT("categories"), Arr);
        return Arr;
    }

    // Find the first entry in a categories array whose "category" field equals
    // the given name (case-insensitive). Returns null if not found.
    const TSharedPtr<FJsonObject>* ClaireonToolLogCategoriesSpec_FindCategory(
        const TArray<TSharedPtr<FJsonValue>>& Categories,
        const FString& Name)
    {
        for (const TSharedPtr<FJsonValue>& Val : Categories)
        {
            const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
            if (!Val.IsValid() || !Val->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid())
            {
                continue;
            }
            FString Cat;
            if ((*ObjPtr)->TryGetStringField(TEXT("category"), Cat) &&
                Cat.Equals(Name, ESearchCase::IgnoreCase))
            {
                return ObjPtr;
            }
        }
        return nullptr;
    }

    // Returns true if the categories array is sorted by name (case-insensitive).
    bool ClaireonToolLogCategoriesSpec_IsSortedByName(
        const TArray<TSharedPtr<FJsonValue>>& Categories)
    {
        FString PrevName;
        for (const TSharedPtr<FJsonValue>& Val : Categories)
        {
            const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
            if (!Val.IsValid() || !Val->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid())
            {
                continue;
            }
            FString Cat;
            if (!(*ObjPtr)->TryGetStringField(TEXT("category"), Cat))
            {
                continue;
            }
            if (!PrevName.IsEmpty() &&
                Cat.Compare(PrevName, ESearchCase::IgnoreCase) < 0)
            {
                return false;
            }
            PrevName = Cat;
        }
        return true;
    }
} // namespace ClaireonTool_LogCategories_spec_Private
using namespace ClaireonTool_LogCategories_spec_Private;

// ===========================================================================
// Execute with no args: success, categories non-empty, contains LogClaireon,
// every entry has a non-empty verbosity, registered_count > 0.
// Guard: skip-success if GEngine null.
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, LogCategories, NoArgsReturnsRegisteredCategories, UNTEST_TIMEOUTMS(15000))
{
    if (!IsValid(GEngine))
    {
        UE_LOG(LogClaireon, Log,
            TEXT("LogCategories/NoArgsReturnsRegisteredCategories: GEngine null -- skipping."));
        co_return;
    }

    ClaireonTool_LogCategories Tool;
    const IClaireonTool::FToolResult Result =
        Tool.Execute(ClaireonToolLogCategoriesSpec_MakeEmptyArgs());

    UNTEST_ASSERT_FALSE(Result.bIsError);

    const TArray<TSharedPtr<FJsonValue>>* Categories =
        ClaireonToolLogCategoriesSpec_GetCategories(Result);
    UNTEST_ASSERT_PTR(Categories);
    UNTEST_ASSERT_TRUE(Categories->Num() > 0);

    // LogClaireon must be present (it is registered at module startup).
    const TSharedPtr<FJsonObject>* ClaireonEntry =
        ClaireonToolLogCategoriesSpec_FindCategory(*Categories, TEXT("LogClaireon"));
    UNTEST_ASSERT_PTR(ClaireonEntry);

    // Every entry must have a non-empty verbosity field.
    for (const TSharedPtr<FJsonValue>& Val : *Categories)
    {
        const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
        if (!Val.IsValid() || !Val->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid())
        {
            continue;
        }
        FString Verbosity;
        const bool bHasVerbosity = (*ObjPtr)->TryGetStringField(TEXT("verbosity"), Verbosity);
        UNTEST_EXPECT_TRUE(bHasVerbosity);
        UNTEST_EXPECT_FALSE(Verbosity.IsEmpty());
    }

    // registered_count > 0.
    double RegisteredCount = 0.0;
    const bool bHasRegistered =
        Result.Data->TryGetNumberField(TEXT("registered_count"), RegisteredCount);
    UNTEST_ASSERT_TRUE(bHasRegistered);
    UNTEST_ASSERT_TRUE(static_cast<int32>(RegisteredCount) > 0);

    co_return;
}

// ===========================================================================
// Execute with contains = "Claireon": every returned name contains the substring.
// Guard: skip-success if GEngine null.
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, LogCategories, ContainsFilterSubstring, UNTEST_TIMEOUTMS(15000))
{
    if (!IsValid(GEngine))
    {
        UE_LOG(LogClaireon, Log,
            TEXT("LogCategories/ContainsFilterSubstring: GEngine null -- skipping."));
        co_return;
    }

    ClaireonTool_LogCategories Tool;
    const IClaireonTool::FToolResult Result =
        Tool.Execute(ClaireonToolLogCategoriesSpec_MakeContainsArgs(TEXT("Claireon")));

    UNTEST_ASSERT_FALSE(Result.bIsError);

    const TArray<TSharedPtr<FJsonValue>>* Categories =
        ClaireonToolLogCategoriesSpec_GetCategories(Result);
    UNTEST_ASSERT_PTR(Categories);

    // Every returned entry's name must contain "Claireon" (case-insensitive).
    for (const TSharedPtr<FJsonValue>& Val : *Categories)
    {
        const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
        if (!Val.IsValid() || !Val->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid())
        {
            continue;
        }
        FString Cat;
        if (!(*ObjPtr)->TryGetStringField(TEXT("category"), Cat))
        {
            continue;
        }
        const bool bContains = Cat.Contains(TEXT("Claireon"), ESearchCase::IgnoreCase);
        UNTEST_EXPECT_TRUE(bContains);
    }

    co_return;
}

// ===========================================================================
// Counts are integers >= 0; entries sorted by name (case-insensitive).
// Guard: skip-success if GEngine null.
// ===========================================================================
UNTEST_UNIT_OPTS(Claireon, LogCategories, CountsNonNegativeAndSortedByName, UNTEST_TIMEOUTMS(15000))
{
    if (!IsValid(GEngine))
    {
        UE_LOG(LogClaireon, Log,
            TEXT("LogCategories/CountsNonNegativeAndSortedByName: GEngine null -- skipping."));
        co_return;
    }

    ClaireonTool_LogCategories Tool;
    const IClaireonTool::FToolResult Result =
        Tool.Execute(ClaireonToolLogCategoriesSpec_MakeEmptyArgs());

    UNTEST_ASSERT_FALSE(Result.bIsError);

    const TArray<TSharedPtr<FJsonValue>>* Categories =
        ClaireonToolLogCategoriesSpec_GetCategories(Result);
    UNTEST_ASSERT_PTR(Categories);

    // All line_count values must be integers >= 0.
    for (const TSharedPtr<FJsonValue>& Val : *Categories)
    {
        const TSharedPtr<FJsonObject>* ObjPtr = nullptr;
        if (!Val.IsValid() || !Val->TryGetObject(ObjPtr) || !ObjPtr || !(*ObjPtr).IsValid())
        {
            continue;
        }
        double LineCount = -1.0;
        const bool bHasCount = (*ObjPtr)->TryGetNumberField(TEXT("line_count"), LineCount);
        UNTEST_EXPECT_TRUE(bHasCount);
        UNTEST_EXPECT_TRUE(static_cast<int32>(LineCount) >= 0);
    }

    // Entries must be sorted by category name (case-insensitive ascending).
    const bool bSorted = ClaireonToolLogCategoriesSpec_IsSortedByName(*Categories);
    UNTEST_EXPECT_TRUE(bSorted);

    co_return;
}

#endif // WITH_UNTESTED
