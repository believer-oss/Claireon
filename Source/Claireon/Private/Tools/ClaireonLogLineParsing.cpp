// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLogLineParsing.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"
#include "Engine/Engine.h"
#include "Misc/OutputDevice.h"

namespace ClaireonLogLineParsing
{

// ============================================================================
// FLogLineParser::ParseLine
// ============================================================================
//
// Two well-known UE log line shapes:
//   Full:  [2026.01.01-12.00.00:000][  0]LogCategory: Severity: message
//   Bare:  [2026.01.01-12.00.00:000][  0]LogCategory: message
//
// These regexes are semantically identical to the pair that live in
// ClaireonTool_LogTail.cpp (stage 004 removes them from there).
// Construct FRegexPattern per call; no static with dynamic init.

FParsedLogLine FLogLineParser::ParseLine(const FString& RawLine)
{
    FParsedLogLine Result;
    Result.Message = RawLine;

    const FRegexPattern LogPatternFull(
        TEXT("^\\[([^\\]]+)\\]\\[[^\\]]*\\](\\w+):\\s*(Fatal|Error|Warning|Display|Log|Verbose|VeryVerbose):\\s*(.*)$"));
    const FRegexPattern LogPatternBare(
        TEXT("^\\[([^\\]]+)\\]\\[[^\\]]*\\](\\w+):\\s*(.*)$"));

    {
        FRegexMatcher MatcherFull(LogPatternFull, RawLine);
        if (MatcherFull.FindNext())
        {
            Result.Timestamp   = MatcherFull.GetCaptureGroup(1);
            Result.Category    = MatcherFull.GetCaptureGroup(2);
            Result.Severity    = MatcherFull.GetCaptureGroup(3);
            Result.Message     = MatcherFull.GetCaptureGroup(4).TrimStartAndEnd();
            Result.bParsed     = true;
            LastCategory       = Result.Category;
            return Result;
        }
    }

    {
        FRegexMatcher MatcherBare(LogPatternBare, RawLine);
        if (MatcherBare.FindNext())
        {
            Result.Timestamp   = MatcherBare.GetCaptureGroup(1);
            Result.Category    = MatcherBare.GetCaptureGroup(2);
            Result.Severity    = TEXT("Log");
            Result.Message     = MatcherBare.GetCaptureGroup(3).TrimStartAndEnd();
            Result.bParsed     = true;
            LastCategory       = Result.Category;
            return Result;
        }
    }

    // Unparseable line: carry forward category if one was seen.
    if (!LastCategory.IsEmpty())
    {
        Result.Category       = LastCategory;
        Result.bContinuation  = true;
    }
    // else: no category yet; bContinuation stays false (nothing to inherit).

    // Fallback severity detection from line text.
    if (RawLine.Contains(TEXT("Error:")))
    {
        Result.Severity = TEXT("Error");
    }
    else if (RawLine.Contains(TEXT("Warning:")))
    {
        Result.Severity = TEXT("Warning");
    }

    return Result;
}

// ============================================================================
// FCategoryFilter
// ============================================================================

bool FCategoryFilter::IsActive() const
{
    return (Include.Num() > 0) || (Exclude.Num() > 0);
}

bool FCategoryFilter::Passes(const FString& Category) const
{
    // Include check: if Include is non-empty, Category must be in it.
    if (Include.Num() > 0)
    {
        bool bFound = false;
        for (const FString& Entry : Include)
        {
            if (Category.Equals(Entry, ESearchCase::IgnoreCase))
            {
                bFound = true;
                break;
            }
        }
        if (!bFound)
        {
            return false;
        }
    }

    // Exclude check: Category must not be in Exclude.
    for (const FString& Entry : Exclude)
    {
        if (Category.Equals(Entry, ESearchCase::IgnoreCase))
        {
            return false;
        }
    }

    return true;
}

// ============================================================================
// ParseCategoryFilterArgs
// ============================================================================

bool ParseCategoryFilterArgs(const TSharedPtr<FJsonObject>& Arguments,
    const FString& SingleCategoryAlias,
    FCategoryFilter& OutFilter, FString& OutError)
{
    OutFilter = FCategoryFilter{};

    // Parse include_categories array.
    const TArray<TSharedPtr<FJsonValue>>* IncludeArr = nullptr;
    if (Arguments.IsValid() && Arguments->TryGetArrayField(TEXT("include_categories"), IncludeArr))
    {
        for (const TSharedPtr<FJsonValue>& Val : *IncludeArr)
        {
            FString Str;
            if (Val.IsValid() && Val->TryGetString(Str) && !Str.IsEmpty())
            {
                OutFilter.Include.Add(Str.TrimStartAndEnd());
            }
        }
    }

    // Alias conflict check: "category" + non-empty include_categories is an error.
    if (!SingleCategoryAlias.IsEmpty() && OutFilter.Include.Num() > 0)
    {
        OutError = TEXT("Cannot specify both 'category' and 'include_categories'; use one or the other.");
        return false;
    }

    // Apply alias as a one-element include.
    if (!SingleCategoryAlias.IsEmpty())
    {
        OutFilter.Include.Add(SingleCategoryAlias);
    }

    // Parse exclude_categories array.
    const TArray<TSharedPtr<FJsonValue>>* ExcludeArr = nullptr;
    if (Arguments.IsValid() && Arguments->TryGetArrayField(TEXT("exclude_categories"), ExcludeArr))
    {
        for (const TSharedPtr<FJsonValue>& Val : *ExcludeArr)
        {
            FString Str;
            if (Val.IsValid() && Val->TryGetString(Str) && !Str.IsEmpty())
            {
                OutFilter.Exclude.Add(Str.TrimStartAndEnd());
            }
        }
    }

    return true;
}

// ============================================================================
// EnumerateRegisteredLogCategories
// ============================================================================

namespace ClaireonLogLineParsing_Private
{
    // Private output device that captures lines from GEngine->Exec.
    class ClaireonLogLineParsingCapture_OutputDevice : public FOutputDevice
    {
    public:
        TArray<FString> Lines;

        virtual void Serialize(const TCHAR* V, ELogVerbosity::Type /*Verbosity*/, const FName& /*Category*/) override
        {
            Lines.Add(FString(V));
        }
    };
} // namespace ClaireonLogLineParsing_Private
using namespace ClaireonLogLineParsing_Private;

TMap<FName, FString> EnumerateRegisteredLogCategories()
{
    TMap<FName, FString> Result;

    if (!IsValid(GEngine))
    {
        return Result;
    }

    ClaireonLogLineParsingCapture_OutputDevice CaptureDevice;
    GEngine->Exec(nullptr, TEXT("LOG LIST"), CaptureDevice);

    // The engine prints each registered category as:
    //   %-40s  %-12s  %s   (name, verbosity, optional DebugBreak tag)
    // Parse by splitting on whitespace runs; token[0] = name, token[1] = verbosity.
    for (const FString& Line : CaptureDevice.Lines)
    {
        TArray<FString> Tokens;
        Line.ParseIntoArray(Tokens, TEXT(" "), /*InCullEmpty=*/true);
        if (Tokens.Num() < 2)
        {
            continue; // header line or empty; skip
        }

        const FName CategoryName(*Tokens[0]);
        const FString Verbosity = Tokens[1];
        Result.Add(CategoryName, Verbosity);
    }

    return Result;
}

// ============================================================================
// ValidateFilterCategories
// ============================================================================

TArray<FString> ValidateFilterCategories(const FCategoryFilter& Filter,
    const TMap<FName, FString>& Registered, const TSet<FString>& SeenInFile)
{
    TArray<FString> Warnings;

    // Gather all candidate names from both Include and Exclude.
    TArray<FString> AllEntries;
    AllEntries.Append(Filter.Include);
    AllEntries.Append(Filter.Exclude);

    for (const FString& Entry : AllEntries)
    {
        const FName EntryName(*Entry);
        const bool bInRegistered = Registered.Contains(EntryName);

        bool bInSeen = false;
        if (!bInRegistered)
        {
            for (const FString& Seen : SeenInFile)
            {
                if (Entry.Equals(Seen, ESearchCase::IgnoreCase))
                {
                    bInSeen = true;
                    break;
                }
            }
        }

        if (!bInRegistered && !bInSeen)
        {
            Warnings.Add(FString::Printf(
                TEXT("'%s' is not a registered log category and does not appear in the log; use log/categories to list valid categories."),
                *Entry));
        }
    }

    return Warnings;
}

bool IsWellFormedLogTimestamp(const FString& Stamp)
{
    // "YYYY.MM.DD-HH.MM.SS" (19 chars) optionally + ":mmm" (23 chars).
    // Shape-only check: field ranges are the log writer's problem; the
    // consumer needs ordering, which zero-padded digits give for free.
    if (Stamp.Len() != 19 && Stamp.Len() != 23)
    {
        return false;
    }
    static const TCHAR* Template19 = TEXT("dddd.dd.dd-dd.dd.dd");
    for (int32 i = 0; i < 19; ++i)
    {
        if (Template19[i] == TEXT('d'))
        {
            if (!FChar::IsDigit(Stamp[i])) { return false; }
        }
        else if (Stamp[i] != Template19[i])
        {
            return false;
        }
    }
    if (Stamp.Len() == 23)
    {
        if (Stamp[19] != TEXT(':')) { return false; }
        for (int32 i = 20; i < 23; ++i)
        {
            if (!FChar::IsDigit(Stamp[i])) { return false; }
        }
    }
    return true;
}

} // namespace ClaireonLogLineParsing
