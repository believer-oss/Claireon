// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

// Shared log-line parsing and category-filter helpers used by the log tools.
// All functions live in the ClaireonLogLineParsing namespace.
// This header is Private (internal helper; not exported).

namespace ClaireonLogLineParsing
{

// ---------------------------------------------------------------------------
// Parsed log line
// ---------------------------------------------------------------------------

struct FParsedLogLine
{
    FString Timestamp;
    FString Category;       // parsed, or carried-forward for continuations; empty if neither
    FString Severity = TEXT("Log");
    FString Message;        // trimmed payload; == raw line when unparsed
    bool bParsed = false;       // matched one of the two standard shapes
    bool bContinuation = false; // category was inherited via carry-forward
};

// ---------------------------------------------------------------------------
// Stateful sequential parser
// Feed lines in file order; carries the last parsed category forward into
// unparseable (continuation) lines.
// ---------------------------------------------------------------------------

class FLogLineParser
{
public:
    FParsedLogLine ParseLine(const FString& RawLine);
private:
    FString LastCategory;
};

// ---------------------------------------------------------------------------
// Per-call category filter. Case-insensitive matching.
// ---------------------------------------------------------------------------

struct FCategoryFilter
{
    TArray<FString> Include; // empty = include everything
    TArray<FString> Exclude; // empty = exclude nothing

    // Returns true when any entry in either set is present.
    bool IsActive() const;

    // Returns true when:
    //   (Include empty || Category is in Include) && Category is not in Exclude.
    // Matching is case-insensitive.
    bool Passes(const FString& Category) const;
};

// ---------------------------------------------------------------------------
// Parse include_categories / exclude_categories (+ optional single-category
// alias) from tool arguments. Returns false with OutError set on alias
// conflict ("category" provided AND include_categories non-empty).
// ---------------------------------------------------------------------------

bool ParseCategoryFilterArgs(const TSharedPtr<FJsonObject>& Arguments,
    const FString& SingleCategoryAlias /* parsed "category" value or empty */,
    FCategoryFilter& OutFilter, FString& OutError);

// ---------------------------------------------------------------------------
// Enumerate registered log categories via the engine's "LOG LIST" exec.
// Game-thread only. Returns category name -> verbosity string.
// Lines that fail to parse are skipped (tolerant whitespace split).
// Returns an empty map when GEngine is null.
// ---------------------------------------------------------------------------

TMap<FName, FString> EnumerateRegisteredLogCategories();

// ---------------------------------------------------------------------------
// Produce warning strings for filter entries matching neither a registered
// category nor a category seen in the log file.
//
// Exact warning shape:
//   "'<Name>' is not a registered log category and does not appear in the
//    log; use log/categories to list valid categories."
// ---------------------------------------------------------------------------

TArray<FString> ValidateFilterCategories(const FCategoryFilter& Filter,
    const TMap<FName, FString>& Registered, const TSet<FString>& SeenInFile);

} // namespace ClaireonLogLineParsing
