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

FString ClaireonTool_LogSearch::GetOperation() const { return TEXT("log_search"); }

FString ClaireonTool_LogSearch::GetCategory() const
{
	return TEXT("editor");
}

FString ClaireonTool_LogSearch::GetDescription() const
{
    return TEXT("Search the full editor log file with a regex pattern. Returns matched lines with line numbers and optional context. Stateless / read-only / non-session: reads the on-disk log file.");
}

TSharedPtr<FJsonObject> ClaireonTool_LogSearch::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// pattern - required (H6: `query` kwarg accepted as alias).
	TSharedPtr<FJsonObject> PatternProp = MakeShared<FJsonObject>();
	PatternProp->SetStringField(TEXT("type"), TEXT("string"));
	PatternProp->SetStringField(TEXT("description"),
		TEXT("Regex pattern to search for (e.g. 'Error.*Blueprint', 'LogAbilitySystem'). The `query` kwarg is accepted as a synonym."));
	Properties->SetObjectField(TEXT("pattern"), PatternProp);

	// query= as an alias for pattern=. Not required individually because at
	// least one of {pattern, query} must be provided (enforced in Execute).
	TSharedPtr<FJsonObject> QueryAliasProp = MakeShared<FJsonObject>();
	QueryAliasProp->SetStringField(TEXT("type"), TEXT("string"));
	QueryAliasProp->SetStringField(TEXT("description"),
		TEXT("Alias for `pattern`. If both are provided, `pattern` wins."));
	Properties->SetObjectField(TEXT("query"), QueryAliasProp);

	// max_results - optional
	TSharedPtr<FJsonObject> MaxProp = MakeShared<FJsonObject>();
	MaxProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxProp->SetStringField(TEXT("description"),
		TEXT("Maximum number of matches to return (default: 50, max: 200). The response always carries `total_matches` and `truncated` so 'returned < total' is detectable."));
	Properties->SetObjectField(TEXT("max_results"), MaxProp);

	// context_lines - optional
	TSharedPtr<FJsonObject> CtxProp = MakeShared<FJsonObject>();
	CtxProp->SetStringField(TEXT("type"), TEXT("integer"));
	CtxProp->SetStringField(TEXT("description"),
		TEXT("Number of context lines before and after each match (default: 0, max: 5)"));
	Properties->SetObjectField(TEXT("context_lines"), CtxProp);

	// Caller can force the inline response (skipping spill).
	TSharedPtr<FJsonObject> ForceInlineProp = MakeShared<FJsonObject>();
	ForceInlineProp->SetStringField(TEXT("type"), TEXT("boolean"));
	ForceInlineProp->SetStringField(TEXT("description"),
		TEXT("If true, signals downstream consumers (and the spill router via `prefers_inline`) that the caller wants the result on the wire even at the cost of context. Default: false."));
	ForceInlineProp->SetBoolField(TEXT("default"), false);
	Properties->SetObjectField(TEXT("force_inline"), ForceInlineProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// pattern XOR query is required; the schema lists pattern as required for
	// MCP-clients that don't know about the alias, while Execute() accepts either.
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("pattern")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_LogSearch::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// Accept `query` as an alias for `pattern`. `pattern` wins when both are
	// set so existing callers stay byte-identical.
	FString Pattern;
	Arguments->TryGetStringField(TEXT("pattern"), Pattern);
	if (Pattern.IsEmpty())
	{
		Arguments->TryGetStringField(TEXT("query"), Pattern);
	}
	if (Pattern.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: pattern (or `query` alias)"));
	}

	int32 MaxResults = 50;
	Arguments->TryGetNumberField(TEXT("max_results"), MaxResults);
	MaxResults = FMath::Clamp(MaxResults, 1, 200);

	int32 ContextLines = 0;
	Arguments->TryGetNumberField(TEXT("context_lines"), ContextLines);
	ContextLines = FMath::Clamp(ContextLines, 0, 5);

	// Caller-driven preference -- echoed on the response so spill observers
	// and downstream consumers can act on it.
	bool bForceInline = false;
	Arguments->TryGetBoolField(TEXT("force_inline"), bForceInline);

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

	// Explicit `truncated` flag so callers cannot confuse a paginated
	// 0-match-in-page response with a true 0-total-match response. The
	// summary distinguishes "no matches" / "N returned, M total" /
	// "N returned, truncated".
	const bool bTruncated = (TotalMatches > MatchesArray.Num());

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("matches"), MatchesArray);
	Data->SetNumberField(TEXT("total_matches"), TotalMatches);
	Data->SetNumberField(TEXT("total_lines"), AllLines.Num());
	Data->SetStringField(TEXT("pattern"), Pattern);
	Data->SetBoolField(TEXT("truncated"), bTruncated);
	Data->SetNumberField(TEXT("returned"), MatchesArray.Num());
	Data->SetNumberField(TEXT("max_results"), MaxResults);
	// Echo the caller's force_inline preference so spill/observers see it.
	if (bForceInline)
	{
		Data->SetBoolField(TEXT("force_inline"), true);
	}
	// When context_lines>0, mark per_array truncation. The before/after
	// arrays are full-line copies so they are not currently mid-line clipped, but the
	// flag is true whenever the parent response is truncated -- callers should treat
	// it as "context arrays for missing matches are absent" rather than "context is
	// clipped within a single line".
	if (ContextLines > 0)
	{
		Data->SetBoolField(TEXT("truncated_per_array"), bTruncated);
	}

	FString Summary;
	if (TotalMatches == 0)
	{
		Summary = FString::Printf(TEXT("No matches for '%s' in %d log lines"), *Pattern, AllLines.Num());
	}
	else if (bTruncated)
	{
		Summary = FString::Printf(TEXT("%d returned, truncated; %d total matches for '%s' in %d log lines"),
			MatchesArray.Num(), TotalMatches, *Pattern, AllLines.Num());
	}
	else
	{
		Summary = FString::Printf(TEXT("Found %d matches for '%s' in %d log lines (showing %d)"),
			TotalMatches, *Pattern, AllLines.Num(), MatchesArray.Num());
	}

	return MakeSuccessResult(Data, Summary);
}
