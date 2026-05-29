// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LogTail.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "Misc/OutputDeviceRedirector.h"
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

	// Explicit category filter on the parsed line's category field.
	TSharedPtr<FJsonObject> CategoryProp = MakeShared<FJsonObject>();
	CategoryProp->SetStringField(TEXT("type"), TEXT("string"));
	CategoryProp->SetStringField(TEXT("description"),
		TEXT("Filter by parsed category name (case-insensitive, e.g. 'LogBlueprint'). Applied AFTER the rendered-line `filter` regex."));
	Properties->SetObjectField(TEXT("category"), CategoryProp);

	// Explicit severity filter on the parsed line's severity field.
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
	int32 LineCount = 100;
	Arguments->TryGetNumberField(TEXT("lineCount"), LineCount);
	LineCount = FMath::Clamp(LineCount, 1, 1000);

	FString FilterPattern;
	Arguments->TryGetStringField(TEXT("filter"), FilterPattern);

	// Explicit category / severity filters operate on the parsed line fields,
	// not the rendered text, so they survive case-only or punctuation drift.
	FString CategoryFilter;
	Arguments->TryGetStringField(TEXT("category"), CategoryFilter);
	CategoryFilter = CategoryFilter.TrimStartAndEnd();
	FString SeverityFilter;
	Arguments->TryGetStringField(TEXT("severity"), SeverityFilter);
	SeverityFilter = SeverityFilter.TrimStartAndEnd();

	// Locate the log file
	FString CurrentLogPath = FPlatformOutputDevices::GetAbsoluteLogFilename();
	if (!FPaths::FileExists(CurrentLogPath))
	{
		return MakeErrorResult(FString::Printf(TEXT("Log file not found: %s"), *CurrentLogPath));
	}

	// Read the file using shared-read access (FILEREAD_AllowWrite) so we can read
	// while the editor process holds its write lock on the log file
	TArray<FString> AllLines;
	{
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*CurrentLogPath, FILEREAD_AllowWrite));
		if (Reader)
		{
			FString FileContents;
			const int64 FileSize = Reader->TotalSize();
			auto& RawArray = FileContents.GetCharArray();
			// Read as UTF-8 then convert
			TArray<uint8> RawBytes;
			RawBytes.SetNumUninitialized(FileSize);
			Reader->Serialize(RawBytes.GetData(), FileSize);
			Reader->Close();

			// Convert from UTF-8 (UE log files are UTF-8 with BOM)
			FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(RawBytes.GetData()), RawBytes.Num());
			FileContents = FString(Converter.Length(), Converter.Get());
			FileContents.ParseIntoArrayLines(AllLines, false);
		}
	}

	// Apply optional regex filter
	TArray<FString> FilteredLines;
	if (FilterPattern.IsEmpty())
	{
		FilteredLines = AllLines;
	}
	else
	{
		const FRegexPattern Pattern(FilterPattern);
		for (const FString& Line : AllLines)
		{
			FRegexMatcher Matcher(Pattern, Line);
			if (Matcher.FindNext())
			{
				FilteredLines.Add(Line);
			}
		}
	}

	// Take the last N lines
	const int32 StartIdx = FMath::Max(0, FilteredLines.Num() - LineCount);
	TArray<TSharedPtr<FJsonValue>> LinesArray;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	// UE log lines come in two well-known shapes:
	//   1. Standard:  [2026.01.01-12.00.00:000][  0]LogCategory: Verbosity: message
	//   2. Standard (no explicit verbosity, == Log): [2026...][  0]LogCategory: message
	// The original single regex captured Error/Warning only and dropped the third group
	// for any Display/Log/Verbose line. The new pair of regexes always populates
	// timestamp/category/severity (or sets a parse_warning); the fallback below covers
	// any remaining unparseable lines.
	const FRegexPattern LogPatternFull(
		TEXT("^\\[([^\\]]+)\\]\\[[^\\]]*\\](\\w+):\\s*(Fatal|Error|Warning|Display|Log|Verbose|VeryVerbose):\\s*(.*)$"));
	const FRegexPattern LogPatternBare(
		TEXT("^\\[([^\\]]+)\\]\\[[^\\]]*\\](\\w+):\\s*(.*)$"));

	for (int32 i = StartIdx; i < FilteredLines.Num(); ++i)
	{
		const FString& RawLine = FilteredLines[i];

		TSharedPtr<FJsonObject> LineObj = MakeShared<FJsonObject>();

		// Try to parse structured log line
		FString Timestamp;
		FString Category;
		FString Severity = TEXT("Log");
		FString Message = RawLine;
		bool bParseOk = false;

		{
			FRegexMatcher MatcherFull(LogPatternFull, RawLine);
			if (MatcherFull.FindNext())
			{
				Timestamp = MatcherFull.GetCaptureGroup(1);
				Category = MatcherFull.GetCaptureGroup(2);
				Severity = MatcherFull.GetCaptureGroup(3);
				Message = MatcherFull.GetCaptureGroup(4).TrimStartAndEnd();
				bParseOk = true;
			}
			else
			{
				FRegexMatcher MatcherBare(LogPatternBare, RawLine);
				if (MatcherBare.FindNext())
				{
					Timestamp = MatcherBare.GetCaptureGroup(1);
					Category = MatcherBare.GetCaptureGroup(2);
					Severity = TEXT("Log");
					Message = MatcherBare.GetCaptureGroup(3).TrimStartAndEnd();
					bParseOk = true;
				}
			}
		}

		if (!bParseOk)
		{
			// Fallback: check for Error/Warning keywords inside an unparseable line.
			if (RawLine.Contains(TEXT("Error:")))
			{
				Severity = TEXT("Error");
			}
			else if (RawLine.Contains(TEXT("Warning:")))
			{
				Severity = TEXT("Warning");
			}
			LineObj->SetStringField(TEXT("parse_warning"),
				TEXT("Line did not match standard [timestamp][frame]Category: Severity: shape; fields populated best-effort."));
		}

		// Post-parse filters on category + severity (case-insensitive).
		if (!CategoryFilter.IsEmpty() && !Category.Equals(CategoryFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}
		if (!SeverityFilter.IsEmpty() && !Severity.Equals(SeverityFilter, ESearchCase::IgnoreCase))
		{
			continue;
		}

		if (Severity.Equals(TEXT("Error"), ESearchCase::IgnoreCase)) { ErrorCount++; }
		else if (Severity.Equals(TEXT("Warning"), ESearchCase::IgnoreCase)) { WarningCount++; }

		LineObj->SetStringField(TEXT("timestamp"), Timestamp);
		LineObj->SetStringField(TEXT("category"), Category);
		LineObj->SetStringField(TEXT("severity"), Severity);
		LineObj->SetStringField(TEXT("message"), Message);
		LinesArray.Add(MakeShared<FJsonValueObject>(LineObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("lines"), LinesArray);
	Data->SetNumberField(TEXT("total_lines"), LinesArray.Num());
	if (!CategoryFilter.IsEmpty())
	{
		Data->SetStringField(TEXT("category_filter"), CategoryFilter);
	}
	if (!SeverityFilter.IsEmpty())
	{
		Data->SetStringField(TEXT("severity_filter"), SeverityFilter);
	}

	const FString Summary = FString::Printf(TEXT("Last %d log entries (%d errors, %d warnings)"),
		LinesArray.Num(), ErrorCount, WarningCount);

	return MakeSuccessResult(Data, Summary);
}
