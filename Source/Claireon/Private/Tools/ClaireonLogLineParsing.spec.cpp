// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "Tools/ClaireonLogLineParsing.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Engine.h"
#include "ClaireonLog.h"

// ---------------------------------------------------------------------------
// Anonymous-namespace helpers -- all prefixed ClaireonLogLineParsingSpec_ to
// avoid anon-namespace unity-build collisions on linux-build-server-v2.
// UNTEST_ASSERT_*/UNTEST_EXPECT_* must never appear inside lambdas (they
// expand to co_return); helpers that need assertions run inline in the test
// coroutine.
// ---------------------------------------------------------------------------
namespace ClaireonLogLineParsing_spec_Private
{
    // Build a TSharedPtr<FJsonObject> with an include_categories array.
    TSharedPtr<FJsonObject> ClaireonLogLineParsingSpec_MakeArgsWithInclude(
        const TArray<FString>& Categories)
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& C : Categories)
        {
            Arr.Add(MakeShared<FJsonValueString>(C));
        }
        Args->SetArrayField(TEXT("include_categories"), Arr);
        return Args;
    }

    // Build a TSharedPtr<FJsonObject> with an exclude_categories array.
    TSharedPtr<FJsonObject> ClaireonLogLineParsingSpec_MakeArgsWithExclude(
        const TArray<FString>& Categories)
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
        TArray<TSharedPtr<FJsonValue>> Arr;
        for (const FString& C : Categories)
        {
            Arr.Add(MakeShared<FJsonValueString>(C));
        }
        Args->SetArrayField(TEXT("exclude_categories"), Arr);
        return Args;
    }

    // Build args with both include and exclude.
    TSharedPtr<FJsonObject> ClaireonLogLineParsingSpec_MakeArgsBoth(
        const TArray<FString>& Include, const TArray<FString>& Exclude)
    {
        TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();

        TArray<TSharedPtr<FJsonValue>> IncArr;
        for (const FString& C : Include)
        {
            IncArr.Add(MakeShared<FJsonValueString>(C));
        }
        Args->SetArrayField(TEXT("include_categories"), IncArr);

        TArray<TSharedPtr<FJsonValue>> ExArr;
        for (const FString& C : Exclude)
        {
            ExArr.Add(MakeShared<FJsonValueString>(C));
        }
        Args->SetArrayField(TEXT("exclude_categories"), ExArr);
        return Args;
    }
} // namespace ClaireonLogLineParsing_spec_Private
using namespace ClaireonLogLineParsing_spec_Private;

// ===========================================================================
// FLogLineParser -- line shape parsing
// ===========================================================================

// Full-shape line: timestamp, category, severity, message all populated.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, FullShapeParses, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FLogLineParser Parser;
    const FString Raw = TEXT("[2026.01.01-12.00.00:000][  0]LogFSSpawner: Warning: Something went wrong");
    const ClaireonLogLineParsing::FParsedLogLine Line = Parser.ParseLine(Raw);

    UNTEST_ASSERT_TRUE(Line.bParsed);
    UNTEST_ASSERT_FALSE(Line.bContinuation);
    UNTEST_ASSERT_STREQ(*Line.Timestamp, TEXT("2026.01.01-12.00.00:000"));
    UNTEST_ASSERT_STREQ(*Line.Category, TEXT("LogFSSpawner"));
    UNTEST_ASSERT_STREQ(*Line.Severity, TEXT("Warning"));
    UNTEST_ASSERT_STREQ(*Line.Message, TEXT("Something went wrong"));
    co_return;
}

// Bare-shape line (no explicit severity): severity defaults to "Log".
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, BareShapeSeverityDefaultsToLog, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FLogLineParser Parser;
    const FString Raw = TEXT("[2026.01.01-12.00.00:001][  1]LogEngine: Editor engine started");
    const ClaireonLogLineParsing::FParsedLogLine Line = Parser.ParseLine(Raw);

    UNTEST_ASSERT_TRUE(Line.bParsed);
    UNTEST_ASSERT_FALSE(Line.bContinuation);
    UNTEST_ASSERT_STREQ(*Line.Category, TEXT("LogEngine"));
    UNTEST_ASSERT_STREQ(*Line.Severity, TEXT("Log"));
    UNTEST_ASSERT_STREQ(*Line.Message, TEXT("Editor engine started"));
    co_return;
}

// Unparseable line after a parsed line: bContinuation = true, inherits category.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, ContinuationInheritsCategory, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FLogLineParser Parser;

    // First: valid parsed line that sets LastCategory.
    const FString ValidRaw = TEXT("[2026.01.01-00.00.00:000][  0]LogFoo: Error: first line");
    const ClaireonLogLineParsing::FParsedLogLine First = Parser.ParseLine(ValidRaw);
    UNTEST_ASSERT_TRUE(First.bParsed);

    // Second: unparseable (stack trace style).
    const FString ContinuationRaw = TEXT("  at SomeFunction() in SomeFile.cpp:42");
    const ClaireonLogLineParsing::FParsedLogLine Cont = Parser.ParseLine(ContinuationRaw);

    UNTEST_ASSERT_FALSE(Cont.bParsed);
    UNTEST_ASSERT_TRUE(Cont.bContinuation);
    UNTEST_ASSERT_STREQ(*Cont.Category, TEXT("LogFoo"));
    co_return;
}

// Unparseable line before any parsed line: empty category, bContinuation == false.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, UnparseableBeforeFirstParsedLine, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FLogLineParser Parser;
    const FString Raw = TEXT("   some raw startup output before UE log format kicks in");
    const ClaireonLogLineParsing::FParsedLogLine Line = Parser.ParseLine(Raw);

    UNTEST_ASSERT_FALSE(Line.bParsed);
    // bContinuation should be false: nothing to inherit yet.
    UNTEST_ASSERT_FALSE(Line.bContinuation);
    UNTEST_ASSERT_TRUE(Line.Category.IsEmpty());
    co_return;
}

// Fallback severity: unparsed line containing "Error:" -> Severity "Error".
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, FallbackSeverityError, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FLogLineParser Parser;
    const FString Raw = TEXT("Error: something bad happened (non-standard format)");
    const ClaireonLogLineParsing::FParsedLogLine Line = Parser.ParseLine(Raw);

    UNTEST_ASSERT_FALSE(Line.bParsed);
    UNTEST_ASSERT_STREQ(*Line.Severity, TEXT("Error"));
    co_return;
}

// Fallback severity: unparsed line containing "Warning:" -> Severity "Warning".
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, FallbackSeverityWarning, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FLogLineParser Parser;
    const FString Raw = TEXT("Warning: something may be wrong");
    const ClaireonLogLineParsing::FParsedLogLine Line = Parser.ParseLine(Raw);

    UNTEST_ASSERT_FALSE(Line.bParsed);
    UNTEST_ASSERT_STREQ(*Line.Severity, TEXT("Warning"));
    co_return;
}

// ===========================================================================
// FCategoryFilter
// ===========================================================================

// Include-only: only matching category passes.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, FilterIncludeOnly, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FCategoryFilter Filter;
    Filter.Include.Add(TEXT("LogFoo"));

    UNTEST_ASSERT_TRUE(Filter.IsActive());
    UNTEST_ASSERT_TRUE(Filter.Passes(TEXT("LogFoo")));
    UNTEST_ASSERT_FALSE(Filter.Passes(TEXT("LogBar")));
    UNTEST_ASSERT_FALSE(Filter.Passes(TEXT("")));
    co_return;
}

// Exclude-only: excluded category fails; others pass including empty.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, FilterExcludeOnly, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FCategoryFilter Filter;
    Filter.Exclude.Add(TEXT("LogNoise"));

    UNTEST_ASSERT_TRUE(Filter.IsActive());
    UNTEST_ASSERT_FALSE(Filter.Passes(TEXT("LogNoise")));
    UNTEST_ASSERT_TRUE(Filter.Passes(TEXT("LogFoo")));
    UNTEST_ASSERT_TRUE(Filter.Passes(TEXT("")));
    co_return;
}

// Both include and exclude: must be in Include AND not in Exclude.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, FilterBothCombined, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FCategoryFilter Filter;
    Filter.Include.Add(TEXT("LogFoo"));
    Filter.Include.Add(TEXT("LogBar"));
    Filter.Exclude.Add(TEXT("LogBar"));

    UNTEST_ASSERT_TRUE(Filter.IsActive());
    UNTEST_ASSERT_TRUE(Filter.Passes(TEXT("LogFoo")));   // in Include, not in Exclude
    UNTEST_ASSERT_FALSE(Filter.Passes(TEXT("LogBar")));  // in Include but also in Exclude
    UNTEST_ASSERT_FALSE(Filter.Passes(TEXT("LogBaz")));  // not in Include
    co_return;
}

// Case-insensitive matching.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, FilterCaseInsensitive, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FCategoryFilter Filter;
    Filter.Include.Add(TEXT("logfoo"));

    UNTEST_ASSERT_TRUE(Filter.Passes(TEXT("LogFoo")));
    UNTEST_ASSERT_TRUE(Filter.Passes(TEXT("LOGFOO")));
    UNTEST_ASSERT_TRUE(Filter.Passes(TEXT("logfoo")));
    co_return;
}

// Inactive filter passes everything including empty category.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, FilterInactivePassesAll, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FCategoryFilter Filter;

    UNTEST_ASSERT_FALSE(Filter.IsActive());
    UNTEST_ASSERT_TRUE(Filter.Passes(TEXT("")));
    UNTEST_ASSERT_TRUE(Filter.Passes(TEXT("LogAnything")));
    co_return;
}

// ===========================================================================
// ParseCategoryFilterArgs
// ===========================================================================

// include_categories array is parsed correctly.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, ParseArgsIncludeArray, UNTEST_TIMEOUTMS(5000))
{
    TSharedPtr<FJsonObject> Args = ClaireonLogLineParsingSpec_MakeArgsWithInclude(
        { TEXT("LogFoo"), TEXT("LogBar") });

    ClaireonLogLineParsing::FCategoryFilter Filter;
    FString Error;
    const bool bOk = ClaireonLogLineParsing::ParseCategoryFilterArgs(Args, TEXT(""), Filter, Error);

    UNTEST_ASSERT_TRUE(bOk);
    UNTEST_ASSERT_EQ(Filter.Include.Num(), 2);
    UNTEST_ASSERT_STREQ(*Filter.Include[0], TEXT("LogFoo"));
    UNTEST_ASSERT_STREQ(*Filter.Include[1], TEXT("LogBar"));
    UNTEST_ASSERT_EQ(Filter.Exclude.Num(), 0);
    co_return;
}

// exclude_categories array is parsed correctly.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, ParseArgsExcludeArray, UNTEST_TIMEOUTMS(5000))
{
    TSharedPtr<FJsonObject> Args = ClaireonLogLineParsingSpec_MakeArgsWithExclude(
        { TEXT("LogNoise"), TEXT("LogSlate") });

    ClaireonLogLineParsing::FCategoryFilter Filter;
    FString Error;
    const bool bOk = ClaireonLogLineParsing::ParseCategoryFilterArgs(Args, TEXT(""), Filter, Error);

    UNTEST_ASSERT_TRUE(bOk);
    UNTEST_ASSERT_EQ(Filter.Exclude.Num(), 2);
    UNTEST_ASSERT_EQ(Filter.Include.Num(), 0);
    co_return;
}

// SingleCategoryAlias maps to a one-element Include.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, ParseArgsAliasMapsToInclude, UNTEST_TIMEOUTMS(5000))
{
    TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();

    ClaireonLogLineParsing::FCategoryFilter Filter;
    FString Error;
    const bool bOk = ClaireonLogLineParsing::ParseCategoryFilterArgs(
        Args, TEXT("LogFSSpawner"), Filter, Error);

    UNTEST_ASSERT_TRUE(bOk);
    UNTEST_ASSERT_EQ(Filter.Include.Num(), 1);
    UNTEST_ASSERT_STREQ(*Filter.Include[0], TEXT("LogFSSpawner"));
    co_return;
}

// Alias + non-empty include_categories -> false with error message.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, ParseArgsAliasConflictErrors, UNTEST_TIMEOUTMS(5000))
{
    TSharedPtr<FJsonObject> Args = ClaireonLogLineParsingSpec_MakeArgsWithInclude(
        { TEXT("LogFoo") });

    ClaireonLogLineParsing::FCategoryFilter Filter;
    FString Error;
    const bool bOk = ClaireonLogLineParsing::ParseCategoryFilterArgs(
        Args, TEXT("LogBar") /* alias */, Filter, Error);

    UNTEST_ASSERT_FALSE(bOk);
    UNTEST_ASSERT_FALSE(Error.IsEmpty());
    co_return;
}

// ===========================================================================
// ValidateFilterCategories
// ===========================================================================

// Name present in Registered does not warn.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, ValidateNoWarnForRegistered, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FCategoryFilter Filter;
    Filter.Include.Add(TEXT("LogFoo"));

    TMap<FName, FString> Registered;
    Registered.Add(FName(TEXT("LogFoo")), TEXT("Log"));

    TSet<FString> SeenInFile;
    const TArray<FString> Warnings = ClaireonLogLineParsing::ValidateFilterCategories(
        Filter, Registered, SeenInFile);

    UNTEST_ASSERT_EQ(Warnings.Num(), 0);
    co_return;
}

// Name only in SeenInFile (not in Registered) does not warn.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, ValidateNoWarnForSeenInFile, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FCategoryFilter Filter;
    Filter.Include.Add(TEXT("LogOnlyInFile"));

    TMap<FName, FString> Registered;
    TSet<FString> SeenInFile;
    SeenInFile.Add(TEXT("LogOnlyInFile"));

    const TArray<FString> Warnings = ClaireonLogLineParsing::ValidateFilterCategories(
        Filter, Registered, SeenInFile);

    UNTEST_ASSERT_EQ(Warnings.Num(), 0);
    co_return;
}

// Bogus name (neither registered nor in file) produces exactly the expected warning shape.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, ValidateWarnsBogusCategory, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FCategoryFilter Filter;
    Filter.Include.Add(TEXT("LogFooo"));

    TMap<FName, FString> Registered;
    TSet<FString> SeenInFile;

    const TArray<FString> Warnings = ClaireonLogLineParsing::ValidateFilterCategories(
        Filter, Registered, SeenInFile);

    UNTEST_ASSERT_EQ(Warnings.Num(), 1);
    const FString Expected = TEXT("'LogFooo' is not a registered log category and does not appear in the log; use log/categories to list valid categories.");
    UNTEST_ASSERT_STREQ(*Warnings[0], *Expected);
    co_return;
}

// Both Include and Exclude entries are checked.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, ValidateChecksExcludeEntries, UNTEST_TIMEOUTMS(5000))
{
    ClaireonLogLineParsing::FCategoryFilter Filter;
    Filter.Exclude.Add(TEXT("LogBogusExclude"));

    TMap<FName, FString> Registered;
    TSet<FString> SeenInFile;

    const TArray<FString> Warnings = ClaireonLogLineParsing::ValidateFilterCategories(
        Filter, Registered, SeenInFile);

    UNTEST_ASSERT_EQ(Warnings.Num(), 1);
    UNTEST_ASSERT_TRUE(Warnings[0].Contains(TEXT("LogBogusExclude")));
    co_return;
}

// ===========================================================================
// EnumerateRegisteredLogCategories
// ===========================================================================

// When GEngine is present, result is non-empty and contains LogClaireon.
// When GEngine is null, skip with a note.
UNTEST_UNIT_OPTS(Claireon, LogLineParsing, EnumerateRegisteredCategories, UNTEST_TIMEOUTMS(10000))
{
    if (!IsValid(GEngine))
    {
        UE_LOG(LogClaireon, Log, TEXT("EnumerateRegisteredCategories: GEngine is null, skipping."));
        co_return;
    }

    const TMap<FName, FString> Categories = ClaireonLogLineParsing::EnumerateRegisteredLogCategories();

    UNTEST_ASSERT_TRUE(Categories.Num() > 0);
    UNTEST_ASSERT_TRUE(Categories.Contains(FName(TEXT("LogClaireon"))));
    co_return;
}

#endif // WITH_UNTESTED
