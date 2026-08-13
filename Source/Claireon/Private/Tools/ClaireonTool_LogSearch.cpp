// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LogSearch.h"
#include "Tools/ClaireonLogLineParsing.h"
#include "ClaireonLog.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "Misc/Paths.h"

// ===========================================================================
// Encoding-aware log decoding (WI-15).
//
// Editor logs are usually UTF-8, but test runners and -UTF16LOG produce
// UTF-16 files. The old code decoded raw bytes unconditionally with
// FUTF8ToTCHAR, turning UTF-16 logs into NUL-riddled garbage so every
// pattern returned 0 matches. These helpers BOM-sniff (UTF-16LE / UTF-16BE /
// UTF-8), mirroring FFileHelper::BufferToString semantics, and fall back to
// an alternating-NUL heuristic for BOM-less UTF-16.
//
// External linkage (named namespace, no statics on the two entry points) so
// ClaireonLogSearchEncodingTests.cpp can drive them directly without a
// header change.
// ===========================================================================
namespace ClaireonLogSearchEncoding
{

// Decode a UTF-16 payload (no BOM included) into an FString.
// Drops a trailing odd byte, if any.
static FString LogSearchEncoding_DecodeUtf16Payload(const uint8* Bytes, int64 NumBytes, bool bBigEndian)
{
	const int32 NumUnits = static_cast<int32>(NumBytes / 2);
	TArray<UTF16CHAR> Units;
	Units.SetNumUninitialized(NumUnits);
	for (int32 UnitIdx = 0; UnitIdx < NumUnits; ++UnitIdx)
	{
		const uint8 B0 = Bytes[2 * UnitIdx];
		const uint8 B1 = Bytes[2 * UnitIdx + 1];
		Units[UnitIdx] = bBigEndian
			? static_cast<UTF16CHAR>((static_cast<uint16>(B0) << 8) | B1)
			: static_cast<UTF16CHAR>((static_cast<uint16>(B1) << 8) | B0);
	}
	FUTF16ToTCHAR Converter(Units.GetData(), Units.Num());
	return FString(Converter.Length(), Converter.Get());
}

// Heuristic for BOM-less UTF-16: mostly-ASCII UTF-16 text has a NUL in every
// other byte (odd offsets for LE, even offsets for BE), while UTF-8 log text
// contains no NULs at all. Samples up to the first 4 KiB.
static bool LogSearchEncoding_DetectBomlessUtf16(const uint8* Bytes, int64 NumBytes, bool& bOutBigEndian)
{
	const int64 SampleBytes = FMath::Min<int64>(NumBytes, 4096);
	const int64 NumPairs = SampleBytes / 2;
	if (NumPairs < 4)
	{
		return false; // not enough signal to call it
	}

	int64 EvenZeros = 0; // NUL at even offset -> consistent with UTF-16BE ASCII
	int64 OddZeros = 0;  // NUL at odd offset  -> consistent with UTF-16LE ASCII
	for (int64 PairIdx = 0; PairIdx < NumPairs; ++PairIdx)
	{
		if (Bytes[2 * PairIdx] == 0)
		{
			++EvenZeros;
		}
		if (Bytes[2 * PairIdx + 1] == 0)
		{
			++OddZeros;
		}
	}

	// Require at least 40% of sampled pairs to show the pattern, and a clear
	// dominance of one parity over the other (rules out all-zero binary junk).
	const int64 Threshold = (NumPairs * 2) / 5;
	if (OddZeros >= Threshold && OddZeros > EvenZeros * 4)
	{
		bOutBigEndian = false;
		return true;
	}
	if (EvenZeros >= Threshold && EvenZeros > OddZeros * 4)
	{
		bOutBigEndian = true;
		return true;
	}
	return false;
}

FString DecodeLogBytes(const uint8* Bytes, int64 NumBytes)
{
	if (!Bytes || NumBytes <= 0)
	{
		return FString();
	}

	// BOM sniff first (mirrors FFileHelper::BufferToString).
	if (NumBytes >= 2 && Bytes[0] == 0xFF && Bytes[1] == 0xFE)
	{
		return LogSearchEncoding_DecodeUtf16Payload(Bytes + 2, NumBytes - 2, /*bBigEndian=*/false);
	}
	if (NumBytes >= 2 && Bytes[0] == 0xFE && Bytes[1] == 0xFF)
	{
		return LogSearchEncoding_DecodeUtf16Payload(Bytes + 2, NumBytes - 2, /*bBigEndian=*/true);
	}
	if (NumBytes >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
	{
		// UTF-8 BOM: skip it and decode the rest as UTF-8.
		Bytes += 3;
		NumBytes -= 3;
	}
	else
	{
		// No BOM: check for BOM-less UTF-16 before assuming UTF-8.
		bool bBigEndian = false;
		if (LogSearchEncoding_DetectBomlessUtf16(Bytes, NumBytes, bBigEndian))
		{
			return LogSearchEncoding_DecodeUtf16Payload(Bytes, NumBytes, bBigEndian);
		}
	}

	FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(Bytes), static_cast<int32>(NumBytes));
	return FString(Converter.Length(), Converter.Get());
}

// C6 hardening: maximum bytes read from the tail of the log file. A log search almost
// always wants recent activity, and bounding the read keeps memory and regex cost
// predictable no matter how large the on-disk log has grown. This is also the fix for
// the defect this replaces: FileSize is int64 (a long editor session's log can exceed
// 2 GiB) but the byte buffer below is indexed by int32, so reading the whole file
// unconditionally could narrow a >2 GiB size to a garbage/negative int32 and overrun
// the allocation. This bound is chosen well under INT32_MAX so the cast below can
// never overflow, with generous headroom (tens of thousands of lines at typical
// editor log line lengths).
static constexpr int64 MaxLogSearchTailBytes = 64LL * 1024 * 1024; // 64 MiB

// Testable core of ReadLogFileLines: same contract, but the tail bound is an explicit
// parameter so tests can exercise truncation without writing a 64 MiB fixture. External
// linkage (named namespace) so ClaireonLogSearchEncodingTests.cpp can drive it directly.
//
// OutTruncated is true when FilePath exceeded MaxTailBytes and the read was cut to its
// tail. When true, the returned lines (and therefore any 1-based line numbers a caller
// derives from them) are relative to the first COMPLETE line inside that tail window,
// not to the true start of the file -- earlier lines are not visible to this read at all.
bool ReadLogFileLinesBounded(const FString& FilePath, int64 MaxTailBytes, TArray<FString>& OutLines,
	bool& OutTruncated, FString& OutError)
{
	OutLines.Reset();
	OutTruncated = false;

	// Shared-read access (FILEREAD_AllowWrite) so we can read while the
	// editor process holds its write lock on the live log file.
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*FilePath, FILEREAD_AllowWrite));
	if (!Reader)
	{
		OutError = FString::Printf(TEXT("Failed to open log file for reading: %s"), *FilePath);
		return false;
	}

	const int64 FileSize = Reader->TotalSize();
	if (FileSize < 0)
	{
		OutError = FString::Printf(TEXT("Failed to determine log file size: %s"), *FilePath);
		Reader->Close();
		return false;
	}

	// Bound the read to the tail of the file. Round the cut point DOWN to an even
	// offset so a UTF-16 log (2-byte code units, BOM'd or not -- the BOM is itself 2
	// bytes, so payload-relative and file-absolute parity coincide) never gets its
	// byte-pairing shifted by one: that would byte-swap every code unit from the cut
	// to EOF, not just the first, since UTF-16 has no self-resynchronizing byte
	// pattern the way UTF-8 does.
	int64 ReadStart = FMath::Max<int64>(0, FileSize - MaxTailBytes);
	ReadStart -= (ReadStart % 2);
	OutTruncated = ReadStart > 0;

	if (OutTruncated)
	{
		Reader->Seek(ReadStart);
		if (Reader->IsError() || Reader->Tell() != ReadStart)
		{
			OutError = FString::Printf(TEXT("Failed to seek log file: %s"), *FilePath);
			Reader->Close();
			return false;
		}
	}

	const int64 BytesToRead = FileSize - ReadStart;
	// BytesToRead <= MaxTailBytes by construction (the production caller below passes a
	// 64 MiB bound, well under INT32_MAX), so this cast cannot overflow -- unlike the
	// unbounded FileSize this replaces.
	TArray<uint8> RawBytes;
	RawBytes.SetNumUninitialized(static_cast<int32>(BytesToRead));
	if (BytesToRead > 0)
	{
		Reader->Serialize(RawBytes.GetData(), BytesToRead);
	}
	const bool bReadOk = !Reader->IsError();
	Reader->Close();
	if (!bReadOk)
	{
		OutError = FString::Printf(TEXT("Failed to read log file: %s"), *FilePath);
		return false;
	}

	FString FileContents = DecodeLogBytes(RawBytes.GetData(), RawBytes.Num());

	if (OutTruncated)
	{
		// Byte 0 of this window is almost certainly mid-line (or mid-UTF-8-sequence,
		// which decodes to a handful of garbage/replacement characters at worst).
		// Either way that leading fragment is not a real line, so drop everything up
		// to and including the first line break before parsing.
		int32 FirstNewline = INDEX_NONE;
		if (FileContents.FindChar(TEXT('\n'), FirstNewline))
		{
			FileContents = FileContents.Mid(FirstNewline + 1);
		}
		else
		{
			// The entire tail window is one line longer than MaxTailBytes -- nothing
			// complete to salvage.
			FileContents.Reset();
		}
	}

	FileContents.ParseIntoArrayLines(OutLines, false);
	return true;
}

bool ReadLogFileLines(const FString& FilePath, TArray<FString>& OutLines, bool& OutTruncated, FString& OutError)
{
	return ReadLogFileLinesBounded(FilePath, MaxLogSearchTailBytes, OutLines, OutTruncated, OutError);
}

} // namespace ClaireonLogSearchEncoding

FString ClaireonTool_LogSearch::GetOperation() const { return TEXT("log_search"); }

FString ClaireonTool_LogSearch::GetCategory() const
{
	return TEXT("editor");
}

FString ClaireonTool_LogSearch::GetDescription() const
{
    // C6 hardening: "the full editor log file" stopped being true once the read was bounded
    // to a tail window (see MaxLogSearchTailBytes in the .cpp) -- a log over that size used
    // to silently hide everything before the tail; now this says so plainly instead of
    // promising full-file coverage it cannot deliver on a large log.
    // Kept under the 400-char P5 description cap that DescriptionLint enforces; the
    // per-field detail (exact bound, window-relative numbering) lives in the schema.
    return TEXT("Search the editor log file with a regex pattern. Reads only the most recent ~64 MiB, so ")
           TEXT("earlier matches are invisible; when log_truncated_to_tail is true, line numbers and ")
           TEXT("total_lines are window-relative, not file-relative. Returns matched lines with optional ")
           TEXT("context, filterable via include_categories / exclude_categories. Handles UTF-8 and ")
           TEXT("UTF-16. Stateless, read-only, non-session.");
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
		TEXT("Number of context lines before and after each match (default: 0, max: 5). Context lines are neighbors within the category-filtered view; lines excluded by category filters do not appear as context."));
	Properties->SetObjectField(TEXT("context_lines"), CtxProp);

	// include_categories - optional
	TSharedPtr<FJsonObject> IncludeProp = MakeShared<FJsonObject>();
	IncludeProp->SetStringField(TEXT("type"), TEXT("array"));
	{
		TSharedPtr<FJsonObject> ItemsObj = MakeShared<FJsonObject>();
		ItemsObj->SetStringField(TEXT("type"), TEXT("string"));
		IncludeProp->SetObjectField(TEXT("items"), ItemsObj);
	}
	IncludeProp->SetStringField(TEXT("description"),
		TEXT("If non-empty, only lines whose parsed log category is in this list are searched. Case-insensitive. Continuation lines inherit the most recent parsed category. Omit or pass [] to include all categories. Use log/categories to list valid values."));
	Properties->SetObjectField(TEXT("include_categories"), IncludeProp);

	// exclude_categories - optional
	TSharedPtr<FJsonObject> ExcludeProp = MakeShared<FJsonObject>();
	ExcludeProp->SetStringField(TEXT("type"), TEXT("array"));
	{
		TSharedPtr<FJsonObject> ItemsObj = MakeShared<FJsonObject>();
		ItemsObj->SetStringField(TEXT("type"), TEXT("string"));
		ExcludeProp->SetObjectField(TEXT("items"), ItemsObj);
	}
	ExcludeProp->SetStringField(TEXT("description"),
		TEXT("Lines whose parsed log category is in this list are removed before regex matching. Case-insensitive. May be combined with include_categories. Omit or pass [] to exclude nothing. Use log/categories to list valid values."));
	Properties->SetObjectField(TEXT("exclude_categories"), ExcludeProp);

	// since / before - optional time-range bounds (P2-16).
	TSharedPtr<FJsonObject> SinceProp = MakeShared<FJsonObject>();
	SinceProp->SetStringField(TEXT("type"), TEXT("string"));
	SinceProp->SetStringField(TEXT("description"),
		TEXT("Only search lines stamped at or after this UE log timestamp ('YYYY.MM.DD-HH.MM.SS' or 'YYYY.MM.DD-HH.MM.SS:mmm'). Log timestamps exist only when the editor runs with -LogTimes (the default); lines without a usable stamp are searched anyway and counted in a warning. Continuation lines inherit the most recent stamped line's time."));
	Properties->SetObjectField(TEXT("since"), SinceProp);

	TSharedPtr<FJsonObject> BeforeProp = MakeShared<FJsonObject>();
	BeforeProp->SetStringField(TEXT("type"), TEXT("string"));
	BeforeProp->SetStringField(TEXT("description"),
		TEXT("Only search lines stamped strictly before this UE log timestamp (same format and stampless behavior as `since`)."));
	Properties->SetObjectField(TEXT("before"), BeforeProp);

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

	// Parse category filter. No single-category alias on this tool; pass empty string.
	ClaireonLogLineParsing::FCategoryFilter CategoryFilter;
	{
		FString FilterError;
		if (!ClaireonLogLineParsing::ParseCategoryFilterArgs(Arguments, TEXT(""), CategoryFilter, FilterError))
		{
			return MakeErrorResult(FilterError);
		}
	}

	// Parse time-range bounds (P2-16). Malformed bounds error immediately: a
	// bound that silently matched nothing would be the silent-no-op class this
	// parameter exists to avoid.
	FString Since, Before;
	Arguments->TryGetStringField(TEXT("since"), Since);
	Arguments->TryGetStringField(TEXT("before"), Before);
	for (const FString* Bound : { &Since, &Before })
	{
		if (!Bound->IsEmpty() && !ClaireonLogLineParsing::IsWellFormedLogTimestamp(*Bound))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Malformed timestamp '%s': expected 'YYYY.MM.DD-HH.MM.SS' or 'YYYY.MM.DD-HH.MM.SS:mmm' (as written by -LogTimes log lines)."),
				**Bound));
		}
	}
	const bool bTimeFilterActive = !Since.IsEmpty() || !Before.IsEmpty();

	// Locate the log file
	FString CurrentLogPath = FPlatformOutputDevices::GetAbsoluteLogFilename();
	if (!FPaths::FileExists(CurrentLogPath))
	{
		return MakeErrorResult(FString::Printf(TEXT("Log file not found: %s"), *CurrentLogPath));
	}

	// Read + decode up to the last MaxLogSearchTailBytes of the file (see that
	// constant's comment for why the read is bounded rather than reading the whole
	// file). Encoding is BOM-sniffed (UTF-8 / UTF-16LE / UTF-16BE, with a heuristic
	// for BOM-less UTF-16) so -UTF16LOG and test-runner logs are searchable. Read
	// failures are reported instead of silently returning zero lines.
	TArray<FString> AllLines;
	bool bLogTruncatedToTail = false;
	{
		FString ReadError;
		if (!ClaireonLogSearchEncoding::ReadLogFileLines(CurrentLogPath, AllLines, bLogTruncatedToTail, ReadError))
		{
			return MakeErrorResult(ReadError);
		}
	}

	const int32 TotalFileLines = AllLines.Num();

	// ---------------------------------------------------------------------------
	// Category filter path (only when the filter is active).
	// Build a filtered view: (line text, original 1-based line number).
	// Lines that do not pass the category filter are excluded and counted.
	// The no-filter path stays byte-identical in behavior: AllLines is used
	// directly and no parsing is performed.
	// ---------------------------------------------------------------------------

	// FilteredLines[i] = index into AllLines (0-based) for the i-th survivor.
	TArray<int32> FilteredIndices;
	int32 CategoryExcludedCount = 0;
	int32 TimeExcludedCount = 0;
	int32 StamplessSearchedCount = 0;
	TSet<FString> SeenCategoriesInFile;
	const bool bCategoryFilterActive = CategoryFilter.IsActive();
	// The parse-and-filter view runs when EITHER filter is on; without both,
	// the no-filter path below stays byte-identical to the original.
	bool bFilterActive = bCategoryFilterActive || bTimeFilterActive;

	if (bFilterActive)
	{
		ClaireonLogLineParsing::FLogLineParser Parser;
		FilteredIndices.Reserve(AllLines.Num());

		// Carry the most recent stamp forward so continuation lines inherit
		// their parent line's time (P2-16), mirroring the category carry.
		FString LastTimestamp;

		for (int32 i = 0; i < AllLines.Num(); ++i)
		{
			const ClaireonLogLineParsing::FParsedLogLine Parsed = Parser.ParseLine(AllLines[i]);
			if (!Parsed.Category.IsEmpty())
			{
				SeenCategoriesInFile.Add(Parsed.Category);
			}
			if (!Parsed.Timestamp.IsEmpty()
				&& ClaireonLogLineParsing::IsWellFormedLogTimestamp(Parsed.Timestamp))
			{
				LastTimestamp = Parsed.Timestamp;
			}

			if (!CategoryFilter.Passes(Parsed.Category))
			{
				++CategoryExcludedCount;
				continue;
			}

			if (bTimeFilterActive)
			{
				if (LastTimestamp.IsEmpty())
				{
					// No usable stamp (own or inherited): searched anyway and
					// counted, so a stampless log filters to "everything plus a
					// warning naming -LogTimes" instead of silently to nothing.
					++StamplessSearchedCount;
				}
				else if ((!Since.IsEmpty() && LastTimestamp < Since)
					|| (!Before.IsEmpty() && !(LastTimestamp < Before)))
				{
					++TimeExcludedCount;
					continue;
				}
			}

			FilteredIndices.Add(i);
		}
	}

	// ---------------------------------------------------------------------------
	// Apply regex pattern over the (possibly filtered) view.
	// Match reporting uses the original 1-based line numbers.
	// Context lines are neighbors in the filtered view.
	// ---------------------------------------------------------------------------
	const FRegexPattern RegexPattern(Pattern);
	TArray<TSharedPtr<FJsonValue>> MatchesArray;
	int32 TotalMatches = 0;

	if (bFilterActive)
	{
		// Operate on FilteredIndices. Context is the filtered view's neighbors.
		for (int32 fi = 0; fi < FilteredIndices.Num(); ++fi)
		{
			const int32 RawIdx = FilteredIndices[fi];
			FRegexMatcher Matcher(RegexPattern, AllLines[RawIdx]);
			if (Matcher.FindNext())
			{
				TotalMatches++;

				if (MatchesArray.Num() >= MaxResults)
				{
					continue; // count but don't collect
				}

				TSharedPtr<FJsonObject> MatchObj = MakeShared<FJsonObject>();
				MatchObj->SetNumberField(TEXT("line_number"), RawIdx + 1); // 1-based original
				MatchObj->SetStringField(TEXT("text"), AllLines[RawIdx]);

				if (ContextLines > 0)
				{
					TArray<TSharedPtr<FJsonValue>> BeforeArray;
					for (int32 fj = FMath::Max(0, fi - ContextLines); fj < fi; ++fj)
					{
						BeforeArray.Add(MakeShared<FJsonValueString>(AllLines[FilteredIndices[fj]]));
					}
					MatchObj->SetArrayField(TEXT("before"), BeforeArray);

					TArray<TSharedPtr<FJsonValue>> AfterArray;
					for (int32 fj = fi + 1; fj <= FMath::Min(FilteredIndices.Num() - 1, fi + ContextLines); ++fj)
					{
						AfterArray.Add(MakeShared<FJsonValueString>(AllLines[FilteredIndices[fj]]));
					}
					MatchObj->SetArrayField(TEXT("after"), AfterArray);
				}

				MatchesArray.Add(MakeShared<FJsonValueObject>(MatchObj));
			}
		}
	}
	else
	{
		// No-filter path: byte-identical behavior to the original implementation.
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
	}

	const bool bTruncated = (TotalMatches > MatchesArray.Num());

	// ---------------------------------------------------------------------------
	// Validate filter categories against registered and seen-in-file sets.
	// Only meaningful when the filter is active.
	// ---------------------------------------------------------------------------
	TArray<FString> Warnings;
	if (bCategoryFilterActive)
	{
		const TMap<FName, FString> Registered = ClaireonLogLineParsing::EnumerateRegisteredLogCategories();
		Warnings = ClaireonLogLineParsing::ValidateFilterCategories(CategoryFilter, Registered, SeenCategoriesInFile);
	}
	if (bTimeFilterActive && StamplessSearchedCount > 0)
	{
		Warnings.Add(FString::Printf(
			TEXT("%d line(s) carried no usable timestamp and were searched despite since/before. ")
			TEXT("Log timestamps require the editor to run with -LogTimes; without them a time ")
			TEXT("filter cannot exclude anything."),
			StamplessSearchedCount));
	}

	// ---------------------------------------------------------------------------
	// Build response summary suffix for excluded counts.
	// ---------------------------------------------------------------------------
	FString ExcludedSuffix;
	if (bFilterActive && CategoryExcludedCount > 0)
	{
		ExcludedSuffix = FString::Printf(TEXT(" (%d excluded by category filters)"), CategoryExcludedCount);
	}
	if (bTimeFilterActive && TimeExcludedCount > 0)
	{
		ExcludedSuffix += FString::Printf(TEXT(" (%d excluded by since/before)"), TimeExcludedCount);
	}

	// ---------------------------------------------------------------------------
	// Build summary.
	// ---------------------------------------------------------------------------
	// C6 hardening: when the log exceeded the tail-read bound, say so in the one
	// channel every family surfaces (see GetContentAsString/Summary), not just in Data.
	const FString TailSuffix = bLogTruncatedToTail
		? TEXT(" [log exceeds the ~64 MiB tail-read bound; searched only the most recent portion]")
		: FString();

	FString Summary;
	if (TotalMatches == 0)
	{
		Summary = FString::Printf(TEXT("No matches for '%s' in %d log lines%s%s"),
			*Pattern, TotalFileLines, *ExcludedSuffix, *TailSuffix);
	}
	else if (bTruncated)
	{
		Summary = FString::Printf(TEXT("%d returned, truncated; %d total matches for '%s' in %d log lines%s%s"),
			MatchesArray.Num(), TotalMatches, *Pattern, TotalFileLines, *ExcludedSuffix, *TailSuffix);
	}
	else
	{
		Summary = FString::Printf(TEXT("Found %d matches for '%s' in %d log lines (showing %d)%s%s"),
			TotalMatches, *Pattern, TotalFileLines, MatchesArray.Num(), *ExcludedSuffix, *TailSuffix);
	}

	// ---------------------------------------------------------------------------
	// Build response data object.
	// ---------------------------------------------------------------------------
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("matches"), MatchesArray);
	Data->SetNumberField(TEXT("total_matches"), TotalMatches);
	Data->SetNumberField(TEXT("total_lines"), TotalFileLines);
	Data->SetStringField(TEXT("pattern"), Pattern);
	Data->SetBoolField(TEXT("truncated"), bTruncated);
	Data->SetNumberField(TEXT("returned"), MatchesArray.Num());
	Data->SetNumberField(TEXT("max_results"), MaxResults);
	// True when the on-disk log exceeded the ~64 MiB tail-read bound: total_lines and
	// every line_number in matches[] are then relative to the read window, not the
	// whole file (see GetDescription()).
	Data->SetBoolField(TEXT("log_truncated_to_tail"), bLogTruncatedToTail);

	// Echo the caller's force_inline preference so spill/observers see it.
	if (bForceInline)
	{
		Data->SetBoolField(TEXT("force_inline"), true);
	}

	// When context_lines>0, mark per_array truncation.
	if (ContextLines > 0)
	{
		Data->SetBoolField(TEXT("truncated_per_array"), bTruncated);
	}

	// Time filter response fields (P2-16, only when a bound was supplied).
	if (bTimeFilterActive)
	{
		if (!Since.IsEmpty())  { Data->SetStringField(TEXT("since"), Since); }
		if (!Before.IsEmpty()) { Data->SetStringField(TEXT("before"), Before); }
		Data->SetNumberField(TEXT("time_excluded_count"), TimeExcludedCount);
		Data->SetNumberField(TEXT("stampless_searched_count"), StamplessSearchedCount);
	}

	// Category filter response fields (only when filter was active).
	if (bFilterActive)
	{
		Data->SetNumberField(TEXT("filtered_lines"), FilteredIndices.Num());
		Data->SetNumberField(TEXT("category_excluded_count"), CategoryExcludedCount);

		// Echo the filter params that were applied.
		{
			TArray<TSharedPtr<FJsonValue>> IncArr;
			for (const FString& C : CategoryFilter.Include)
			{
				IncArr.Add(MakeShared<FJsonValueString>(C));
			}
			Data->SetArrayField(TEXT("include_categories"), IncArr);
		}
		{
			TArray<TSharedPtr<FJsonValue>> ExArr;
			for (const FString& C : CategoryFilter.Exclude)
			{
				ExArr.Add(MakeShared<FJsonValueString>(C));
			}
			Data->SetArrayField(TEXT("exclude_categories"), ExArr);
		}

		// The category-filter nudge moved off Data.hint onto the structured Result.Hint
		// channel; it is attached at the return below so it can be latched.

		if (Warnings.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> WarnArr;
			for (const FString& W : Warnings)
			{
				WarnArr.Add(MakeShared<FJsonValueString>(W));
			}
			Data->SetArrayField(TEXT("warnings"), WarnArr);
		}
	}

	FToolResult Result = MakeSuccessResult(Data, Summary);

	// LATCHED per session by hint code. This hint teaches a PARAMETER -- omit the category
	// filters -- and that lesson transfers the moment it lands, so repeating it on every call
	// of a sweep is exactly the bulk-loop noise that forced a latch onto the
	// get_editor_property nudge. args echoes the original call with the filters removed, so it
	// stays directly callable.
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
