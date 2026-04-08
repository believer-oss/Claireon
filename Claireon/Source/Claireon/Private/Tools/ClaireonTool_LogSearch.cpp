// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LogSearch.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "Misc/Paths.h"

FString ClaireonTool_LogSearch::GetName() const
{
	return TEXT("claireon.log_search");
}

FString ClaireonTool_LogSearch::GetCategory() const
{
	return TEXT("build");
}

FString ClaireonTool_LogSearch::GetDescription() const
{
	return TEXT("Search the full editor log file with a regex pattern. Returns matched lines with line numbers and optional context.");
}

TSharedPtr<FJsonObject> ClaireonTool_LogSearch::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// pattern - required
	TSharedPtr<FJsonObject> PatternProp = MakeShared<FJsonObject>();
	PatternProp->SetStringField(TEXT("type"), TEXT("string"));
	PatternProp->SetStringField(TEXT("description"),
		TEXT("Regex pattern to search for (e.g. 'Error.*Blueprint', 'LogAbilitySystem')"));
	Properties->SetObjectField(TEXT("pattern"), PatternProp);

	// max_results - optional
	TSharedPtr<FJsonObject> MaxProp = MakeShared<FJsonObject>();
	MaxProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxProp->SetStringField(TEXT("description"),
		TEXT("Maximum number of matches to return (default: 50, max: 200)"));
	Properties->SetObjectField(TEXT("max_results"), MaxProp);

	// context_lines - optional
	TSharedPtr<FJsonObject> CtxProp = MakeShared<FJsonObject>();
	CtxProp->SetStringField(TEXT("type"), TEXT("integer"));
	CtxProp->SetStringField(TEXT("description"),
		TEXT("Number of context lines before and after each match (default: 0, max: 5)"));
	Properties->SetObjectField(TEXT("context_lines"), CtxProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("pattern")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_LogSearch::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString Pattern;
	if (!Arguments->TryGetStringField(TEXT("pattern"), Pattern) || Pattern.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: pattern"));
	}

	int32 MaxResults = 50;
	Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
	MaxResults = FMath::Clamp(MaxResults, 1, 200);

	int32 ContextLines = 0;
	Arguments->TryGetNumberField(TEXT("context_lines"), ContextLines);
	ContextLines = FMath::Clamp(ContextLines, 0, 5);

	// Locate the log file
	FString CurrentLogPath = FPlatformOutputDevices::GetAbsoluteLogFilename();
	if (!FPaths::FileExists(CurrentLogPath))
	{
		return MakeErrorResult(FString::Printf(TEXT("Log file not found: %s"), *CurrentLogPath));
	}

	// Read the full file using shared-read access (FILEREAD_AllowWrite) so we can read
	// while the editor process holds its write lock on the log file
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

			FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(RawBytes.GetData()), RawBytes.Num());
			FString FileContents(Converter.Length(), Converter.Get());
			FileContents.ParseIntoArrayLines(AllLines, false);
		}
	}

	// Apply regex pattern
	const FRegexPattern RegexPattern(Pattern);
	TArray<TSharedPtr<FJsonValue>> MatchesArray;
	int32 TotalMatches = 0;

	for (int32 i = 0; i < AllLines.Num(); ++i)
	{
		FRegexMatcher Matcher(RegexPattern, AllLines[i]);
		if (Matcher.FindNext())
		{
			TotalMatches++;

			if (MatchesArray.Num() >= MaxResults)
			{
				continue; // count but don't collect
			}

			TSharedPtr<FJsonObject> MatchObj = MakeShared<FJsonObject>();
			MatchObj->SetNumberField(TEXT("line_number"), i + 1); // 1-based
			MatchObj->SetStringField(TEXT("text"), AllLines[i]);

			// Add context lines if requested
			if (ContextLines > 0)
			{
				TArray<TSharedPtr<FJsonValue>> BeforeArray;
				for (int32 j = FMath::Max(0, i - ContextLines); j < i; ++j)
				{
					BeforeArray.Add(MakeShared<FJsonValueString>(AllLines[j]));
				}
				MatchObj->SetArrayField(TEXT("before"), BeforeArray);

				TArray<TSharedPtr<FJsonValue>> AfterArray;
				for (int32 j = i + 1; j <= FMath::Min(AllLines.Num() - 1, i + ContextLines); ++j)
				{
					AfterArray.Add(MakeShared<FJsonValueString>(AllLines[j]));
				}
				MatchObj->SetArrayField(TEXT("after"), AfterArray);
			}

			MatchesArray.Add(MakeShared<FJsonValueObject>(MatchObj));
		}
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("matches"), MatchesArray);
	Data->SetNumberField(TEXT("total_matches"), TotalMatches);
	Data->SetNumberField(TEXT("total_lines"), AllLines.Num());
	Data->SetStringField(TEXT("pattern"), Pattern);

	const FString Summary = FString::Printf(TEXT("Found %d matches for '%s' in %d log lines (showing %d)"),
		TotalMatches, *Pattern, AllLines.Num(), MatchesArray.Num());

	return MakeSuccessResult(Data, Summary);
}
