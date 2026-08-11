// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LogCategories.h"
#include "Tools/ClaireonLogLineParsing.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FString ClaireonTool_LogCategories::GetCategory() const { return TEXT("log"); }
FString ClaireonTool_LogCategories::GetOperation() const { return TEXT("categories"); }

FString ClaireonTool_LogCategories::GetDescription() const
{
    return TEXT("List all registered log categories with verbosity and line counts from the current editor log. Stateless / read-only / non-session: enumerates the engine log registry and scans the on-disk log file.");
}

TSharedPtr<FJsonObject> ClaireonTool_LogCategories::GetInputSchema() const
{
    TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
    Schema->SetStringField(TEXT("type"), TEXT("object"));

    TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

    // contains (optional)
    TSharedPtr<FJsonObject> ContainsProp = MakeShared<FJsonObject>();
    ContainsProp->SetStringField(TEXT("type"), TEXT("string"));
    ContainsProp->SetStringField(TEXT("description"),
        TEXT("Case-insensitive substring filter on category name (e.g. 'Claireon', 'Spawner')."));
    Properties->SetObjectField(TEXT("contains"), ContainsProp);

    Schema->SetObjectField(TEXT("properties"), Properties);

    return Schema;
}

IClaireonTool::FToolResult ClaireonTool_LogCategories::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    // -----------------------------------------------------------------------
    // Step 1: enumerate registered log categories via "LOG LIST" exec.
    // EnumerateRegisteredLogCategories() returns an empty map when GEngine is
    // null; treat that as a soft error rather than a hard one so headless
    // callers see a useful message.
    // -----------------------------------------------------------------------
    const TMap<FName, FString> Registered =
        ClaireonLogLineParsing::EnumerateRegisteredLogCategories();

    if (Registered.Num() == 0)
    {
        return MakeErrorResult(
            TEXT("log/categories: GEngine is null or returned no registered categories. "
                 "The tool must run on the game thread with a live engine."));
    }

    // -----------------------------------------------------------------------
    // Step 2: scan the current log file to collect per-category line counts.
    // -----------------------------------------------------------------------
    bool bLogFileFound = false;
    TMap<FString, int32> LineCounts; // category name (as FString) -> count

    const FString CurrentLogPath = FPlatformOutputDevices::GetAbsoluteLogFilename();
    if (FPaths::FileExists(CurrentLogPath))
    {
        TUniquePtr<FArchive> Reader(
            IFileManager::Get().CreateFileReader(*CurrentLogPath, FILEREAD_AllowWrite));
        if (Reader)
        {
            bLogFileFound = true;

            const int64 FileSize = Reader->TotalSize();
            TArray<uint8> RawBytes;
            RawBytes.SetNumUninitialized(FileSize);
            Reader->Serialize(RawBytes.GetData(), FileSize);
            Reader->Close();

            // Convert from UTF-8 (UE log files are UTF-8 with BOM).
            FUTF8ToTCHAR Converter(
                reinterpret_cast<const ANSICHAR*>(RawBytes.GetData()), RawBytes.Num());
            FString FileContents(Converter.Length(), Converter.Get());

            TArray<FString> AllLines;
            FileContents.ParseIntoArrayLines(AllLines, false);

            ClaireonLogLineParsing::FLogLineParser Parser;
            for (const FString& RawLine : AllLines)
            {
                const ClaireonLogLineParsing::FParsedLogLine Parsed = Parser.ParseLine(RawLine);

                // Count lines toward their (possibly carried-forward) category.
                // Skip lines with no category at all (pre-first-line or unparseable
                // with nothing to carry forward).
                if (!Parsed.Category.IsEmpty())
                {
                    int32& Count = LineCounts.FindOrAdd(Parsed.Category, 0);
                    ++Count;
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Step 3: merge registered + file-seen categories.
    // -----------------------------------------------------------------------

    // Build a unified set of category names (case-preserved from the registry
    // for registered entries; as-seen from the file for unregistered ones).
    // Key: lower-case name for dedup; Value: { display name, verbosity, line_count, bUnregistered }.
    struct FCategoryEntry
    {
        FString DisplayName;
        FString Verbosity;
        int32   LineCount = 0;
        bool    bUnregistered = false;
    };

    TMap<FString, FCategoryEntry> Merged; // lower-case key -> entry

    // Add all registered categories first.
    for (const TPair<FName, FString>& Pair : Registered)
    {
        const FString DisplayName = Pair.Key.ToString();
        const FString LowerKey = DisplayName.ToLower();

        FCategoryEntry Entry;
        Entry.DisplayName   = DisplayName;
        Entry.Verbosity     = Pair.Value;
        Entry.bUnregistered = false;

        // Look up line count (case-insensitive).
        for (const TPair<FString, int32>& LC : LineCounts)
        {
            if (LC.Key.Equals(DisplayName, ESearchCase::IgnoreCase))
            {
                Entry.LineCount = LC.Value;
                break;
            }
        }

        Merged.Add(LowerKey, Entry);
    }

    // Add file-seen categories that are not in the registry.
    for (const TPair<FString, int32>& LC : LineCounts)
    {
        const FString LowerKey = LC.Key.ToLower();
        if (!Merged.Contains(LowerKey))
        {
            FCategoryEntry Entry;
            Entry.DisplayName   = LC.Key;
            Entry.Verbosity     = TEXT("Unknown");
            Entry.LineCount     = LC.Value;
            Entry.bUnregistered = true;
            Merged.Add(LowerKey, Entry);
        }
    }

    // -----------------------------------------------------------------------
    // Step 4: optional "contains" substring filter (case-insensitive).
    // -----------------------------------------------------------------------
    FString ContainsFilter;
    if (Arguments.IsValid())
    {
        Arguments->TryGetStringField(TEXT("contains"), ContainsFilter);
    }
    ContainsFilter = ContainsFilter.TrimStartAndEnd();

    // -----------------------------------------------------------------------
    // Step 5: sort by category name (case-insensitive) and build output array.
    // -----------------------------------------------------------------------

    // Collect all entries into a sorted array.
    TArray<FCategoryEntry> Sorted;
    Sorted.Reserve(Merged.Num());
    for (const TPair<FString, FCategoryEntry>& KV : Merged)
    {
        if (ContainsFilter.IsEmpty() ||
            KV.Value.DisplayName.Contains(ContainsFilter, ESearchCase::IgnoreCase))
        {
            Sorted.Add(KV.Value);
        }
    }

    Sorted.Sort([](const FCategoryEntry& A, const FCategoryEntry& B)
    {
        return A.DisplayName.Compare(B.DisplayName, ESearchCase::IgnoreCase) < 0;
    });

    // Build the JSON categories array.
    TArray<TSharedPtr<FJsonValue>> CategoriesArray;
    CategoriesArray.Reserve(Sorted.Num());
    for (const FCategoryEntry& Entry : Sorted)
    {
        TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
        Obj->SetStringField(TEXT("category"),    Entry.DisplayName);
        Obj->SetStringField(TEXT("verbosity"),   Entry.Verbosity);
        Obj->SetNumberField(TEXT("line_count"),  Entry.LineCount);
        if (Entry.bUnregistered)
        {
            Obj->SetBoolField(TEXT("unregistered"), true);
        }
        CategoriesArray.Add(MakeShared<FJsonValueObject>(Obj));
    }

    // -----------------------------------------------------------------------
    // Step 6: compute summary counts and build response.
    // -----------------------------------------------------------------------

    // Count categories present in the log (line_count > 0 or seen in file).
    int32 PresentInLogCount = 0;
    for (const FCategoryEntry& Entry : Sorted)
    {
        if (Entry.LineCount > 0)
        {
            ++PresentInLogCount;
        }
    }
    // Also count entries that were filtered out (for the unfiltered summary
    // numbers the spec requires: registered_count is total registry size,
    // present_in_log_count is total log-present size, not filtered subset).
    // We recompute over the unfiltered Merged map.
    int32 RegisteredCount = 0;
    int32 PresentInLogCountTotal = 0;
    for (const TPair<FString, FCategoryEntry>& KV : Merged)
    {
        if (!KV.Value.bUnregistered)
        {
            ++RegisteredCount;
        }
        if (KV.Value.LineCount > 0)
        {
            ++PresentInLogCountTotal;
        }
    }

    TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
    Data->SetArrayField(TEXT("categories"),           CategoriesArray);
    Data->SetNumberField(TEXT("registered_count"),    RegisteredCount);
    Data->SetNumberField(TEXT("present_in_log_count"), PresentInLogCountTotal);
    Data->SetBoolField(TEXT("log_file_found"),        bLogFileFound);

    const FString Summary = FString::Printf(
        TEXT("%d registered categories, %d present in log"),
        RegisteredCount, PresentInLogCountTotal);

    return MakeSuccessResult(Data, Summary);
}
