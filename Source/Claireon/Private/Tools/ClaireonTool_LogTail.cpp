// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LogTail.h"
#include "Tools/ClaireonLogLineParsing.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "Misc/Paths.h"

FString ClaireonTool_LogTail::GetCategory() const { return TEXT("log"); }
FString ClaireonTool_LogTail::GetOperation() const { return TEXT("tail"); }

FString ClaireonTool_LogTail::GetDescription() const
{
    return TEXT("Read recent lines from the editor log with optional filtering. Stateless / read-only / non-session: tails the on-disk log file without opening any asset.");
}

TSharedPtr<FJsonObject> ClaireonTool_LogTail::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// lineCount (optional)
	TSharedPtr<FJsonObject> LineCountProp = MakeShared<FJsonObject>();
	LineCountProp->SetStringField(TEXT("type"), TEXT("integer"));
	LineCountProp->SetStringField(TEXT("description"),
		TEXT("Number of lines to return from the end of the log (default: 100, max: 1000)"));
	Properties->SetObjectField(TEXT("lineCount"), LineCountProp);

	// filter (optional)
	TSharedPtr<FJsonObject> FilterProp = MakeShared<FJsonObject>();
	FilterProp->SetStringField(TEXT("type"), TEXT("string"));
	FilterProp->SetStringField(TEXT("description"),
		TEXT("Regex pattern to filter log lines (e.g. 'Error|Warning', 'LogAbilitySystem'). Matched against the rendered line."));
	Properties->SetObjectField(TEXT("filter"), FilterProp);

	// category (optional) -- alias for a one-element include_categories. Error if both are provided.
	TSharedPtr<FJsonObject> CategoryProp = MakeShared<FJsonObject>();
	CategoryProp->SetStringField(TEXT("type"), TEXT("string"));
	CategoryProp->SetStringField(TEXT("description"),
		TEXT("Alias for a one-element include_categories (case-insensitive, e.g. 'LogFSSpawner'). Error if both category and include_categories are provided. Use log/categories to list valid category names."));
	Properties->SetObjectField(TEXT("category"), CategoryProp);

	// include_categories (optional)
	TSharedPtr<FJsonObject> IncludeCategoriesProp = MakeShared<FJsonObject>();
	IncludeCategoriesProp->SetStringField(TEXT("type"), TEXT("array"));
	{
		TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
		Items->SetStringField(TEXT("type"), TEXT("string"));
		IncludeCategoriesProp->SetObjectField(TEXT("items"), Items);
	}
	IncludeCategoriesProp->SetStringField(TEXT("description"),
		TEXT("Only lines from these categories pass (case-insensitive). Empty/omitted: no include filtering. Combinable with exclude_categories. Use log/categories to list valid category names."));
	Properties->SetObjectField(TEXT("include_categories"), IncludeCategoriesProp);

	// exclude_categories (optional)
	TSharedPtr<FJsonObject> ExcludeCategoriesProp = MakeShared<FJsonObject>();
	ExcludeCategoriesProp->SetStringField(TEXT("type"), TEXT("array"));
	{
		TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
		Items->SetStringField(TEXT("type"), TEXT("string"));
		ExcludeCategoriesProp->SetObjectField(TEXT("items"), Items);
	}
	ExcludeCategoriesProp->SetStringField(TEXT("description"),
		TEXT("Lines from these categories are removed (case-insensitive). Empty/omitted: nothing removed. Combinable with include_categories."));
	Properties->SetObjectField(TEXT("exclude_categories"), ExcludeCategoriesProp);

	// severity (optional)
	TSharedPtr<FJsonObject> SeverityProp = MakeShared<FJsonObject>();
	SeverityProp->SetStringField(TEXT("type"), TEXT("string"));
	SeverityProp->SetStringField(TEXT("description"),
		TEXT("Filter by parsed severity (case-insensitive). Accepts 'Fatal', 'Error', 'Warning', 'Display', 'Log', 'Verbose', 'VeryVerbose'."));
	Properties->SetObjectField(TEXT("severity"), SeverityProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_LogTail::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// --- Parse lineCount ---
	int32 LineCount = 100;
	Arguments->TryGetNumberField(TEXT("lineCount"), LineCount);
	LineCount = FMath::Clamp(LineCount, 1, 1000);

	// --- Parse rendered-line filter regex ---
	FString FilterPattern;
	Arguments->TryGetStringField(TEXT("filter"), FilterPattern);

	// --- Parse severity filter ---
	FString SeverityFilter;
	Arguments->TryGetStringField(TEXT("severity"), SeverityFilter);
	SeverityFilter = SeverityFilter.TrimStartAndEnd();

	// --- Parse category alias (single string) then build FCategoryFilter ---
	FString CategoryAlias;
	Arguments->TryGetStringField(TEXT("category"), CategoryAlias);
	CategoryAlias = CategoryAlias.TrimStartAndEnd();

	ClaireonLogLineParsing::FCategoryFilter CategoryFilter;
	{
		FString FilterError;
		if (!ClaireonLogLineParsing::ParseCategoryFilterArgs(Arguments, CategoryAlias, CategoryFilter, FilterError))
		{
			return MakeErrorResult(FilterError);
		}
	}

	// --- Locate the log file ---
	FString CurrentLogPath = FPlatformOutputDevices::GetAbsoluteLogFilename();
	if (!FPaths::FileExists(CurrentLogPath))
	{
		return MakeErrorResult(FString::Printf(TEXT("Log file not found: %s"), *CurrentLogPath));
	}

	// --- Read the file using shared-read access so we can read while the editor
	//     process holds its write lock on the log file ---
	TArray<FString> AllLines;
	{
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*CurrentLogPath, FILEREAD_AllowWrite));
		if (Reader)
		{
			const int64 FileSize = Reader->TotalSize();
			TArray<uint8> RawBytes;
			RawBytes.SetNumUninitialized(FileSize);
			Reader->Serialize(RawBytes.GetData(), FileSize);
			Reader->Close();

			// Convert from UTF-8 (UE log files are UTF-8 with BOM)
			FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(RawBytes.GetData()), RawBytes.Num());
			FString FileContents(Converter.Length(), Converter.Get());
			FileContents.ParseIntoArrayLines(AllLines, false);
		}
	}

	// --- Single sequential pass over all lines ---
	// Pipeline order per-line:
	//   (a) rendered-line regex filter
	//   (b) severity filter (on parsed severity)
	//   (c) category filter (on parsed/carried-forward category)
	// CategoryExcludedCount counts lines that passed (a) and (b) but failed (c).
	// SeenCategories collects all categories present in the log for validation.

	using namespace ClaireonLogLineParsing;

	FLogLineParser Parser;
	const bool bFilterActive = CategoryFilter.IsActive();

	// Optional regex for rendered-line filter
	TUniquePtr<FRegexPattern> RenderedPattern;
	if (!FilterPattern.IsEmpty())
	{
		RenderedPattern = MakeUnique<FRegexPattern>(FilterPattern);
	}

	TSet<FString> SeenCategories;
	int32 CategoryExcludedCount = 0;

	// Accumulate all survivors from the pipeline before taking last-N.
	struct FParsedSurvivor
	{
		FParsedLogLine Parsed;
	};
	TArray<FParsedSurvivor> Survivors;
	Survivors.Reserve(FMath::Min(AllLines.Num(), LineCount * 4));

	for (const FString& RawLine : AllLines)
	{
		// Parse the line (stateful carry-forward)
		FParsedLogLine Parsed = Parser.ParseLine(RawLine);

		// Collect seen categories for unknown-category validation
		if (!Parsed.Category.IsEmpty())
		{
			SeenCategories.Add(Parsed.Category);
		}

		// (a) Rendered-line regex filter
		if (RenderedPattern)
		{
			FRegexMatcher Matcher(*RenderedPattern, RawLine);
			if (!Matcher.FindNext())
			{
				continue;
			}
		}

		// (b) Severity filter
		if (!SeverityFilter.IsEmpty() && !Parsed.Severity.Equals(SeverityFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		// (c) Category filter -- count exclusions separately
		if (bFilterActive && !CategoryFilter.Passes(Parsed.Category))
		{
			CategoryExcludedCount++;
			continue;
		}

		Survivors.Add({ MoveTemp(Parsed) });
	}

	// --- Take the last lineCount survivors ---
	const int32 StartIdx = FMath::Max(0, Survivors.Num() - LineCount);

	TArray<TSharedPtr<FJsonValue>> LinesArray;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	for (int32 i = StartIdx; i < Survivors.Num(); ++i)
	{
		const FParsedLogLine& Parsed = Survivors[i].Parsed;

		TSharedPtr<FJsonObject> LineObj = MakeShared<FJsonObject>();
		LineObj->SetStringField(TEXT("timestamp"), Parsed.Timestamp);
		LineObj->SetStringField(TEXT("category"), Parsed.Category);
		LineObj->SetStringField(TEXT("severity"), Parsed.Severity);
		LineObj->SetStringField(TEXT("message"), Parsed.Message);

		if (Parsed.bContinuation)
		{
			LineObj->SetBoolField(TEXT("continuation"), true);
		}

		if (!Parsed.bParsed)
		{
			LineObj->SetStringField(TEXT("parse_warning"),
				TEXT("Line did not match standard [timestamp][frame]Category: Severity: shape; fields populated best-effort."));
		}

		if (Parsed.Severity.Equals(TEXT("Error"), ESearchCase::IgnoreCase))    { ErrorCount++; }
		else if (Parsed.Severity.Equals(TEXT("Warning"), ESearchCase::IgnoreCase)) { WarningCount++; }

		LinesArray.Add(MakeShared<FJsonValueObject>(LineObj));
	}

	// --- Build response ---
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("lines"), LinesArray);
	Data->SetNumberField(TEXT("total_lines"), LinesArray.Num());

	// Echo filter params
	if (!CategoryAlias.IsEmpty() && CategoryFilter.Include.Num() == 1 && CategoryFilter.Exclude.Num() == 0)
	{
		// Alias path: keep the legacy category_filter field for back-compat
		Data->SetStringField(TEXT("category_filter"), CategoryAlias);
	}
	else
	{
		if (CategoryFilter.Include.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> IncArr;
			for (const FString& C : CategoryFilter.Include)
			{
				IncArr.Add(MakeShared<FJsonValueString>(C));
			}
			Data->SetArrayField(TEXT("include_categories"), IncArr);
		}
		if (CategoryFilter.Exclude.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> ExcArr;
			for (const FString& C : CategoryFilter.Exclude)
			{
				ExcArr.Add(MakeShared<FJsonValueString>(C));
			}
			Data->SetArrayField(TEXT("exclude_categories"), ExcArr);
		}
	}

	if (!SeverityFilter.IsEmpty())
	{
		Data->SetStringField(TEXT("severity_filter"), SeverityFilter);
	}

	// Category-excluded count + hint (only when filter is active)
	FString Summary;
	if (bFilterActive)
	{
		Data->SetNumberField(TEXT("category_excluded_count"), CategoryExcludedCount);

		if (CategoryExcludedCount > 0)
		{
			// The category-filter nudge moved off Data.hint onto the structured Result.Hint
			// channel; it is attached at the return below so it can be latched.
			Summary = FString::Printf(
				TEXT("Last %d log entries (%d errors, %d warnings) (%d excluded by category filters)"),
				LinesArray.Num(), ErrorCount, WarningCount, CategoryExcludedCount);
		}
		else
		{
			Summary = FString::Printf(TEXT("Last %d log entries (%d errors, %d warnings)"),
				LinesArray.Num(), ErrorCount, WarningCount);
		}

		// Unknown-category warnings (only when filter is active)
		TMap<FName, FString> Registered = EnumerateRegisteredLogCategories();
		TArray<FString> CategoryWarnings = ValidateFilterCategories(CategoryFilter, Registered, SeenCategories);
		if (CategoryWarnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarnArr;
			for (const FString& W : CategoryWarnings)
			{
				WarnArr.Add(MakeShared<FJsonValueString>(W));
			}
			Data->SetArrayField(TEXT("warnings"), WarnArr);
		}
	}
	else
	{
		Summary = FString::Printf(TEXT("Last %d log entries (%d errors, %d warnings)"),
			LinesArray.Num(), ErrorCount, WarningCount);
	}

	FToolResult Result = MakeSuccessResult(Data, Summary);

	// LATCHED per session by hint code, sharing the code with log_search: both teach the same
	// parameter lesson, so a caller that already learned it from one should not be told again
	// by the other. args echoes the original call with the filters removed so it stays
	// directly callable.
	double ExcludedCount = 0.0;
	if (Data.IsValid()
		&& Data->TryGetNumberField(TEXT("category_excluded_count"), ExcludedCount)
		&& ExcludedCount > 0.0
		&& ShouldEmitLatchedHint(TEXT("log_category_filter_excluded")))
	{
		TSharedPtr<FJsonObject> UnfilteredArgs = CloneHintArgs(Arguments);
		UnfilteredArgs->RemoveField(TEXT("include_categories"));
		UnfilteredArgs->RemoveField(TEXT("exclude_categories"));
		Result.Hint = MakeGuidanceHint(GetName(),
			FString::Printf(
				TEXT("%d lines were excluded by category filters. Re-issue without ")
				TEXT("include_categories/exclude_categories to see all lines."),
				static_cast<int32>(ExcludedCount)),
			UnfilteredArgs);
	}
	return Result;
}
