// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LogTail.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformOutputDevices.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"

FString ClaireonTool_LogTail::GetName() const
{
	return TEXT("log_tail");
}

FString ClaireonTool_LogTail::GetCategory() const
{
	return TEXT("build");
}

FString ClaireonTool_LogTail::GetDescription() const
{
	return TEXT("Read recent lines from the editor log with optional filtering");
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
		TEXT("Regex pattern to filter log lines (e.g. 'Error|Warning', 'LogAbilitySystem')"));
	Properties->SetObjectField(TEXT("filter"), FilterProp);

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

	// Locate the log file
	FString CurrentLogPath = FPlatformOutputDevices::GetAbsoluteLogFilename();
	if (!FPaths::FileExists(CurrentLogPath))
	{
		return MakeErrorResult(FString::Printf(TEXT("Log file not found: %s"), *CurrentLogPath));
	}

	// Read the file
	TArray<FString> AllLines;
	FFileHelper::LoadFileToStringArray(AllLines, *CurrentLogPath);

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

	// Simple log line parser: [timestamp][category][verbosity] message
	// Example: [2026.01.01-12.00.00:000][  0]LogTemp: Error: Something failed
	const FRegexPattern LogPattern(TEXT("^\\[([^\\]]+)\\]\\[.*?\\](\\w+):.*?(Error|Warning)?:(.*)$"));

	for (int32 i = StartIdx; i < FilteredLines.Num(); ++i)
	{
		const FString& RawLine = FilteredLines[i];

		TSharedPtr<FJsonObject> LineObj = MakeShared<FJsonObject>();

		// Try to parse structured log line
		FString Timestamp;
		FString Category;
		FString Severity = TEXT("Log");
		FString Message = RawLine;

		FRegexMatcher Matcher(LogPattern, RawLine);
		if (Matcher.FindNext())
		{
			Timestamp = Matcher.GetCaptureGroup(1);
			Category = Matcher.GetCaptureGroup(2);
			const FString SeverityCapture = Matcher.GetCaptureGroup(3);
			Message = Matcher.GetCaptureGroup(4).TrimStartAndEnd();
			if (!SeverityCapture.IsEmpty()) { Severity = SeverityCapture; }
		}
		else
		{
			// Fallback: check for Error/Warning keywords
			if (RawLine.Contains(TEXT("Error:")))
			{
				Severity = TEXT("Error");
			}
			else if (RawLine.Contains(TEXT("Warning:")))
			{
				Severity = TEXT("Warning");
			}
		}

		if (Severity == TEXT("Error")) { ErrorCount++; }
		else if (Severity == TEXT("Warning")) { WarningCount++; }

		LineObj->SetStringField(TEXT("timestamp"), Timestamp);
		LineObj->SetStringField(TEXT("category"), Category);
		LineObj->SetStringField(TEXT("severity"), Severity);
		LineObj->SetStringField(TEXT("message"), Message);
		LinesArray.Add(MakeShared<FJsonValueObject>(LineObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("lines"), LinesArray);
	Data->SetNumberField(TEXT("total_lines"), LinesArray.Num());

	const FString Summary = FString::Printf(TEXT("Last %d log entries (%d errors, %d warnings)"),
		LinesArray.Num(), ErrorCount, WarningCount);

	return MakeSuccessResult(Data, Summary);
}
