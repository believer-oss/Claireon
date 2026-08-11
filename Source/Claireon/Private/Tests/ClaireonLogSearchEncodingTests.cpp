// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "HAL/PlatformProcess.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tools/ClaireonTool_LogSearch.h"

// ---------------------------------------------------------------------------
// WI-15: editor_log_search encoding support.
//
// Drives the external-linkage decode/read helpers defined in
// ClaireonTool_LogSearch.cpp. The tool itself always reads the live editor
// log path (FPlatformOutputDevices), so the encoding contract is exercised
// through the extracted helpers against temp files written in each encoding.
// ---------------------------------------------------------------------------
namespace ClaireonLogSearchEncoding
{
	FString DecodeLogBytes(const uint8* Bytes, int64 NumBytes);
	bool ReadLogFileLines(const FString& FilePath, TArray<FString>& OutLines, bool& OutTruncated, FString& OutError);
	bool ReadLogFileLinesBounded(const FString& FilePath, int64 MaxTailBytes, TArray<FString>& OutLines,
		bool& OutTruncated, FString& OutError);
}

namespace ClaireonLogSearchEncodingTestsInternal
{

// File-local discriminator prefix (LogSearchEncodingTests_) per module
// convention: anonymous namespaces are not isolation under unity batching.

const TCHAR* LogSearchEncodingTests_MarkerLine = TEXT("LogClaireon: CLAIREON_ENC_MARKER_7f3a payload");
const TCHAR* LogSearchEncodingTests_MarkerPattern = TEXT("CLAIREON_ENC_MARKER_\\w+");

// Three-line log body shared by every encoding variant. ASCII only, so each
// UTF-16 code unit is one byte + one NUL.
FString LogSearchEncodingTests_MakeLogText()
{
	FString Text;
	Text += TEXT("LogTemp: Display: editor boot\r\n");
	Text += LogSearchEncodingTests_MarkerLine;
	Text += TEXT("\r\n");
	Text += TEXT("LogTemp: Display: editor shutdown\r\n");
	return Text;
}

void LogSearchEncodingTests_AppendUtf8(const FString& Text, TArray<uint8>& OutBytes)
{
	FTCHARToUTF8 Converter(*Text, Text.Len());
	OutBytes.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
}

void LogSearchEncodingTests_AppendUtf16(const FString& Text, bool bBigEndian, TArray<uint8>& OutBytes)
{
	for (int32 CharIdx = 0; CharIdx < Text.Len(); ++CharIdx)
	{
		const uint16 Unit = static_cast<uint16>(Text[CharIdx]); // ASCII content: one code unit per TCHAR
		const uint8 Lo = static_cast<uint8>(Unit & 0xFF);
		const uint8 Hi = static_cast<uint8>((Unit >> 8) & 0xFF);
		if (bBigEndian)
		{
			OutBytes.Add(Hi);
			OutBytes.Add(Lo);
		}
		else
		{
			OutBytes.Add(Lo);
			OutBytes.Add(Hi);
		}
	}
}

FString LogSearchEncodingTests_WriteTempLog(const TArray<uint8>& Bytes, bool& bOutSaved)
{
	const FString Path = FPaths::CreateTempFilename(FPlatformProcess::UserTempDir(),
		TEXT("ClaireonLogSearchEnc"), TEXT(".log"));
	bOutSaved = FFileHelper::SaveArrayToFile(Bytes, *Path);
	return Path;
}

// Mirrors the tool's per-line regex search over the decoded lines.
// Bool-returning helper so no UNTEST macro is needed inside loops/lambdas.
int32 LogSearchEncodingTests_CountMatches(const TArray<FString>& Lines, const FString& Pattern)
{
	const FRegexPattern RegexPattern(Pattern);
	int32 Matches = 0;
	for (const FString& Line : Lines)
	{
		FRegexMatcher Matcher(RegexPattern, Line);
		if (Matcher.FindNext())
		{
			++Matches;
		}
	}
	return Matches;
}

// One round trip: write Bytes to a temp log, read+decode it via the tool's
// helper, delete the file (before any assertion can bail), then report.
bool LogSearchEncodingTests_RoundTrip(const TArray<uint8>& Bytes, TArray<FString>& OutLines,
	int32& OutMarkerMatches, FString& OutError)
{
	bool bSaved = false;
	const FString Path = LogSearchEncodingTests_WriteTempLog(Bytes, bSaved);
	if (!bSaved)
	{
		OutError = FString::Printf(TEXT("Failed to write temp log: %s"), *Path);
		return false;
	}

	bool bTruncatedUnused = false;
	const bool bRead = ClaireonLogSearchEncoding::ReadLogFileLines(Path, OutLines, bTruncatedUnused, OutError);

	// Teardown before assertions: never leave temp files behind.
	IFileManager::Get().Delete(*Path, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);

	if (!bRead)
	{
		return false;
	}
	OutMarkerMatches = LogSearchEncodingTests_CountMatches(OutLines, LogSearchEncodingTests_MarkerPattern);
	return true;
}

bool LogSearchEncodingTests_LinesContainMarker(const TArray<FString>& Lines)
{
	for (const FString& Line : Lines)
	{
		if (Line.Equals(LogSearchEncodingTests_MarkerLine))
		{
			return true;
		}
	}
	return false;
}

} // namespace ClaireonLogSearchEncodingTestsInternal

using namespace ClaireonLogSearchEncodingTestsInternal;

// ===========================================================================
// Encoding round trips: the marker must be found in every supported encoding.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, LogSearchEncoding, Utf8FindsMarker, UNTEST_TIMEOUTMS(30000))
{
	TArray<uint8> Bytes;
	LogSearchEncodingTests_AppendUtf8(LogSearchEncodingTests_MakeLogText(), Bytes);

	TArray<FString> Lines;
	int32 MarkerMatches = 0;
	FString Error;
	UNTEST_ASSERT_TRUE(LogSearchEncodingTests_RoundTrip(Bytes, Lines, MarkerMatches, Error));
	UNTEST_EXPECT_EQ(MarkerMatches, 1);
	UNTEST_EXPECT_TRUE(LogSearchEncodingTests_LinesContainMarker(Lines));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LogSearchEncoding, Utf8WithBomFindsMarker, UNTEST_TIMEOUTMS(30000))
{
	TArray<uint8> Bytes;
	Bytes.Add(0xEF);
	Bytes.Add(0xBB);
	Bytes.Add(0xBF);
	LogSearchEncodingTests_AppendUtf8(LogSearchEncodingTests_MakeLogText(), Bytes);

	TArray<FString> Lines;
	int32 MarkerMatches = 0;
	FString Error;
	UNTEST_ASSERT_TRUE(LogSearchEncodingTests_RoundTrip(Bytes, Lines, MarkerMatches, Error));
	UNTEST_EXPECT_EQ(MarkerMatches, 1);
	UNTEST_EXPECT_TRUE(LogSearchEncodingTests_LinesContainMarker(Lines));
	// The BOM must not leak into the first line.
	UNTEST_ASSERT_TRUE(Lines.Num() > 0);
	UNTEST_EXPECT_STREQ(*Lines[0], TEXT("LogTemp: Display: editor boot"));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LogSearchEncoding, Utf16LEWithBomFindsMarker, UNTEST_TIMEOUTMS(30000))
{
	TArray<uint8> Bytes;
	Bytes.Add(0xFF);
	Bytes.Add(0xFE);
	LogSearchEncodingTests_AppendUtf16(LogSearchEncodingTests_MakeLogText(), /*bBigEndian=*/false, Bytes);

	TArray<FString> Lines;
	int32 MarkerMatches = 0;
	FString Error;
	UNTEST_ASSERT_TRUE(LogSearchEncodingTests_RoundTrip(Bytes, Lines, MarkerMatches, Error));
	UNTEST_EXPECT_EQ(MarkerMatches, 1);
	UNTEST_EXPECT_TRUE(LogSearchEncodingTests_LinesContainMarker(Lines));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LogSearchEncoding, Utf16LEWithoutBomFindsMarker, UNTEST_TIMEOUTMS(30000))
{
	// The -UTF16LOG / test-runner case the defect report is about: BOM-less
	// UTF-16LE must be caught by the alternating-NUL heuristic.
	TArray<uint8> Bytes;
	LogSearchEncodingTests_AppendUtf16(LogSearchEncodingTests_MakeLogText(), /*bBigEndian=*/false, Bytes);

	TArray<FString> Lines;
	int32 MarkerMatches = 0;
	FString Error;
	UNTEST_ASSERT_TRUE(LogSearchEncodingTests_RoundTrip(Bytes, Lines, MarkerMatches, Error));
	UNTEST_EXPECT_EQ(MarkerMatches, 1);
	UNTEST_EXPECT_TRUE(LogSearchEncodingTests_LinesContainMarker(Lines));
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LogSearchEncoding, Utf16BEWithBomFindsMarker, UNTEST_TIMEOUTMS(30000))
{
	TArray<uint8> Bytes;
	Bytes.Add(0xFE);
	Bytes.Add(0xFF);
	LogSearchEncodingTests_AppendUtf16(LogSearchEncodingTests_MakeLogText(), /*bBigEndian=*/true, Bytes);

	TArray<FString> Lines;
	int32 MarkerMatches = 0;
	FString Error;
	UNTEST_ASSERT_TRUE(LogSearchEncodingTests_RoundTrip(Bytes, Lines, MarkerMatches, Error));
	UNTEST_EXPECT_EQ(MarkerMatches, 1);
	UNTEST_EXPECT_TRUE(LogSearchEncodingTests_LinesContainMarker(Lines));
	co_return;
}

// ===========================================================================
// Binary junk: graceful no-match, no crash.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, LogSearchEncoding, BinaryJunkGracefulNoMatch, UNTEST_TIMEOUTMS(30000))
{
	// Every byte value 0x00..0xFF repeated: neither a BOM nor an alternating
	// NUL pattern, and not valid UTF-8. Must decode without crashing and the
	// marker pattern must find nothing.
	TArray<uint8> Bytes;
	Bytes.Reserve(4096);
	for (int32 ByteIdx = 0; ByteIdx < 4096; ++ByteIdx)
	{
		Bytes.Add(static_cast<uint8>(ByteIdx % 256));
	}

	TArray<FString> Lines;
	int32 MarkerMatches = 0;
	FString Error;
	UNTEST_ASSERT_TRUE(LogSearchEncodingTests_RoundTrip(Bytes, Lines, MarkerMatches, Error));
	UNTEST_EXPECT_EQ(MarkerMatches, 0);
	co_return;
}

// ===========================================================================
// Direct decode checks and the fail-loud read path.
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, LogSearchEncoding, DecodeUtf16LEHasNoEmbeddedNuls, UNTEST_TIMEOUTMS(30000))
{
	// Regression guard for the original defect: UTF-16LE bytes must decode to
	// the exact source text, not to a NUL-riddled expansion of it.
	const FString Source(LogSearchEncodingTests_MarkerLine);
	TArray<uint8> Bytes;
	Bytes.Add(0xFF);
	Bytes.Add(0xFE);
	LogSearchEncodingTests_AppendUtf16(Source, /*bBigEndian=*/false, Bytes);

	const FString Decoded = ClaireonLogSearchEncoding::DecodeLogBytes(Bytes.GetData(), Bytes.Num());
	UNTEST_EXPECT_EQ(Decoded.Len(), Source.Len());
	UNTEST_EXPECT_STREQ(*Decoded, *Source);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LogSearchEncoding, DecodeEmptyBufferIsEmpty, UNTEST_TIMEOUTMS(30000))
{
	const FString Decoded = ClaireonLogSearchEncoding::DecodeLogBytes(nullptr, 0);
	UNTEST_EXPECT_EQ(Decoded.Len(), 0);
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LogSearchEncoding, ExecuteSmokeOnLiveLog, UNTEST_TIMEOUTMS(30000))
{
	// End-to-end wiring check through the tool's Execute contract. The tool
	// always targets the live editor/commandlet log, so only assert on the
	// branch that applies in this runner.
	ClaireonTool_LogSearch Tool;
	TSharedPtr<FJsonObject> Args = MakeShared<FJsonObject>();
	Args->SetStringField(TEXT("pattern"), TEXT("."));
	Args->SetNumberField(TEXT("max_results"), 1);

	IClaireonTool::FToolResult Result = Tool.Execute(Args);

	const FString LiveLogPath = FPlatformOutputDevices::GetAbsoluteLogFilename();
	if (FPaths::FileExists(LiveLogPath))
	{
		// Live log present: the read+decode path must succeed.
		UNTEST_EXPECT_FALSE(Result.bIsError);
	}
	else
	{
		// No log file in this runner: the documented not-found error applies.
		UNTEST_ASSERT_TRUE(Result.bIsError);
		UNTEST_EXPECT_TRUE(Result.ErrorMessage.Contains(TEXT("Log file not found")));
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LogSearchEncoding, ReadMissingFileFailsLoudly, UNTEST_TIMEOUTMS(30000))
{
	const FString BogusPath = FPaths::Combine(FPlatformProcess::UserTempDir(),
		TEXT("ClaireonLogSearchEnc_DoesNotExist_7f3a.log"));

	TArray<FString> Lines;
	bool bTruncated = true; // must come back false
	FString Error;
	const bool bRead = ClaireonLogSearchEncoding::ReadLogFileLines(BogusPath, Lines, bTruncated, Error);
	UNTEST_ASSERT_FALSE(bRead);
	UNTEST_EXPECT_TRUE(Error.Contains(TEXT("Failed to open log file for reading")));
	UNTEST_EXPECT_TRUE(Error.Contains(BogusPath));
	UNTEST_EXPECT_EQ(Lines.Num(), 0);
	UNTEST_EXPECT_FALSE(bTruncated);
	co_return;
}

// ===========================================================================
// C6 hardening: bounded tail reads. Uses ReadLogFileLinesBounded directly with a
// tiny MaxTailBytes so truncation is exercised without writing a 64 MiB fixture;
// production code always calls through ReadLogFileLines, which pins the real
// bound (MaxLogSearchTailBytes, ~64 MiB, in ClaireonTool_LogSearch.cpp).
// ===========================================================================

UNTEST_UNIT_OPTS(Claireon, LogSearchEncoding, BoundedReadDropsPartialFirstLine, UNTEST_TIMEOUTMS(30000))
{
	TArray<uint8> Bytes;
	LogSearchEncodingTests_AppendUtf8(LogSearchEncodingTests_MakeLogText(), Bytes);

	bool bSaved = false;
	const FString Path = LogSearchEncodingTests_WriteTempLog(Bytes, bSaved);
	UNTEST_ASSERT_TRUE(bSaved);

	// Ground truth: the full, untruncated line set (bound is the whole file, so no cut).
	TArray<FString> FullLines;
	bool bFullTruncated = true;
	FString FullError;
	const bool bFullRead = ClaireonLogSearchEncoding::ReadLogFileLinesBounded(
		Path, Bytes.Num(), FullLines, bFullTruncated, FullError);
	UNTEST_ASSERT_TRUE(bFullRead);
	UNTEST_EXPECT_FALSE(bFullTruncated);
	UNTEST_ASSERT_TRUE(FullLines.Num() >= 2);

	// Force a mid-file cut: bounding to half the byte count always lands inside the
	// second or third line of this 3-line fixture, never exactly on a line boundary.
	const int64 TinyBound = FMath::Max<int64>(1, Bytes.Num() / 2);
	TArray<FString> TailLines;
	bool bTailTruncated = false;
	FString TailError;
	const bool bTailRead = ClaireonLogSearchEncoding::ReadLogFileLinesBounded(
		Path, TinyBound, TailLines, bTailTruncated, TailError);

	IFileManager::Get().Delete(*Path, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);

	UNTEST_ASSERT_TRUE(bTailRead);
	UNTEST_EXPECT_TRUE(bTailTruncated);

	// Every returned tail line must be an EXACT, byte-identical match to one of the
	// trailing lines of the full read -- a corrupted or partial leading fragment would
	// not match, and this is the "no garbage first line" property the C6 fix requires.
	UNTEST_ASSERT_TRUE(TailLines.Num() <= FullLines.Num());
	const int32 Offset = FullLines.Num() - TailLines.Num();
	for (int32 Idx = 0; Idx < TailLines.Num(); ++Idx)
	{
		UNTEST_EXPECT_STREQ(*TailLines[Idx], *FullLines[Offset + Idx]);
	}
	co_return;
}

UNTEST_UNIT_OPTS(Claireon, LogSearchEncoding, BoundedReadPreservesUtf16AlignmentOnTruncation, UNTEST_TIMEOUTMS(30000))
{
	TArray<uint8> Bytes;
	Bytes.Add(0xFF);
	Bytes.Add(0xFE);
	LogSearchEncodingTests_AppendUtf16(LogSearchEncodingTests_MakeLogText(), /*bBigEndian=*/false, Bytes);

	bool bSaved = false;
	const FString Path = LogSearchEncodingTests_WriteTempLog(Bytes, bSaved);
	UNTEST_ASSERT_TRUE(bSaved);

	TArray<FString> FullLines;
	bool bFullTruncated = true;
	FString FullError;
	UNTEST_ASSERT_TRUE(ClaireonLogSearchEncoding::ReadLogFileLinesBounded(
		Path, Bytes.Num(), FullLines, bFullTruncated, FullError));
	UNTEST_EXPECT_FALSE(bFullTruncated);
	UNTEST_ASSERT_TRUE(FullLines.Num() >= 2);

	// Deliberately odd bound: forces the pre-alignment cut candidate to be odd, so this
	// proves the even-offset correction actually engages rather than being dead code. A
	// misaligned cut would byte-swap every UTF-16 code unit from the cut to EOF, which
	// would corrupt the comparison below into non-matching garbage.
	const int64 TinyBound = FMath::Max<int64>(3, (Bytes.Num() / 2) | 1);
	TArray<FString> TailLines;
	bool bTailTruncated = false;
	FString TailError;
	const bool bTailRead = ClaireonLogSearchEncoding::ReadLogFileLinesBounded(
		Path, TinyBound, TailLines, bTailTruncated, TailError);

	IFileManager::Get().Delete(*Path, /*bRequireExists=*/false, /*bEvenReadOnly=*/true);

	UNTEST_ASSERT_TRUE(bTailRead);
	UNTEST_EXPECT_TRUE(bTailTruncated);

	UNTEST_ASSERT_TRUE(TailLines.Num() <= FullLines.Num());
	const int32 Offset = FullLines.Num() - TailLines.Num();
	for (int32 Idx = 0; Idx < TailLines.Num(); ++Idx)
	{
		UNTEST_EXPECT_STREQ(*TailLines[Idx], *FullLines[Offset + Idx]);
	}
	co_return;
}

#endif // WITH_UNTESTED
