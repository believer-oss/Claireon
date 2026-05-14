// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT
#if WITH_UNTESTED

#include "Untest.h"
#include "ClaireonLogCapture.h"
#include "ClaireonLog.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "Logging/LogMacros.h"

#include <atomic>

// File-local namespace (NOT raw `namespace { ... }`) to avoid unity-batched
// symbol collisions per feedback_anon_namespace_unity_collision.md.
namespace ClaireonLogCaptureSpec
{
	static constexpr int32 kStressThreads = 4;
	static constexpr int32 kStressIterations = 50;
	static constexpr int32 kPayloadTemplateCount = 3;

	static const TCHAR* const kStressTemplateStems[kPayloadTemplateCount] = {
		TEXT("ClaireonStressTpl0:"),
		TEXT("ClaireonStressTpl1:"),
		TEXT("ClaireonStressTpl2:"),
	};

	// Per-thread bool helper. No UNTEST_ASSERT_* inside (those expand to
	// co_return and cannot live in a non-coroutine lambda body).
	bool EmitOneStressMessage(int32 Tpl, int32 Payload)
	{
		switch (Tpl)
		{
			case 0:
				UE_LOG(LogClaireon, Warning, TEXT("ClaireonStressTpl0:%d"), Payload);
				return true;
			case 1:
				UE_LOG(LogClaireon, Warning, TEXT("ClaireonStressTpl1:%d"), Payload);
				return true;
			case 2:
				UE_LOG(LogClaireon, Warning, TEXT("ClaireonStressTpl2:%d"), Payload);
				return true;
			default:
				return false;
		}
	}
}

// ---------------------------------------------------------------------------
// Construct.SnapshotsDenylist
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, LogCapture, Construct_SnapshotsDenylist, UNTEST_TIMEOUTMS(10000))
{
	FClaireonLogCapture Capture(ELogVerbosity::Warning);

	// Emit a marker through a denied category (LogStreaming is in the default
	// denylist) and through an allowed one (LogClaireon).
	UE_LOG(LogStreaming, Warning, TEXT("ClaireonDeniedMarker_DoNotCapture"));
	UE_LOG(LogClaireon, Warning, TEXT("ClaireonAllowedMarker_ShouldCapture"));

	const FString Output = Capture.GetCapturedOutput();
	UNTEST_ASSERT_FALSE(Output.Contains(TEXT("ClaireonDeniedMarker_DoNotCapture")));
	UNTEST_ASSERT_TRUE(Output.Contains(TEXT("ClaireonAllowedMarker_ShouldCapture")));
	co_return;
}

// ---------------------------------------------------------------------------
// LogCapture.FiltersByVerbosity
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, LogCapture, FiltersByVerbosity, UNTEST_TIMEOUTMS(10000))
{
	FClaireonLogCapture Capture(ELogVerbosity::Warning);

	UE_LOG(LogClaireon, Display, TEXT("ClaireonVerbosity_Display_Drop"));
	UE_LOG(LogClaireon, Verbose, TEXT("ClaireonVerbosity_Verbose_Drop"));
	UE_LOG(LogClaireon, Warning, TEXT("ClaireonVerbosity_Warning_Keep"));
	UE_LOG(LogClaireon, Error,   TEXT("ClaireonVerbosity_Error_Keep"));

	const FString Output = Capture.GetCapturedOutput();
	UNTEST_ASSERT_FALSE(Output.Contains(TEXT("ClaireonVerbosity_Display_Drop")));
	UNTEST_ASSERT_FALSE(Output.Contains(TEXT("ClaireonVerbosity_Verbose_Drop")));
	UNTEST_ASSERT_TRUE(Output.Contains(TEXT("ClaireonVerbosity_Warning_Keep")));
	UNTEST_ASSERT_TRUE(Output.Contains(TEXT("ClaireonVerbosity_Error_Keep")));
	co_return;
}

// ---------------------------------------------------------------------------
// LogCapture.FiltersByCategory
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, LogCapture, FiltersByCategory, UNTEST_TIMEOUTMS(10000))
{
	FClaireonLogCapture Capture(ELogVerbosity::Warning);

	// LogStreaming is in the default denylist; LogClaireon is not.
	UE_LOG(LogStreaming, Warning, TEXT("ClaireonCategory_Streaming_Drop"));
	UE_LOG(LogClaireon, Warning, TEXT("ClaireonCategory_Claireon_Keep"));

	const FString Output = Capture.GetCapturedOutput();
	UNTEST_ASSERT_FALSE(Output.Contains(TEXT("ClaireonCategory_Streaming_Drop")));
	UNTEST_ASSERT_TRUE(Output.Contains(TEXT("ClaireonCategory_Claireon_Keep")));
	co_return;
}

// ---------------------------------------------------------------------------
// LogCapture.RespectsCap
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, LogCapture, RespectsCap, UNTEST_TIMEOUTMS(15000))
{
	FClaireonLogCapture Capture(ELogVerbosity::Warning);

	const int32 EmitCount = FClaireonLogCapture::MaxCapturedMessages + 50;
	for (int32 i = 0; i < EmitCount; ++i)
	{
		UE_LOG(LogClaireon, Warning, TEXT("ClaireonCapMsg:%d"), i);
	}

	const FString Output = Capture.GetCapturedOutput();
	UNTEST_ASSERT_TRUE(Output.Contains(TEXT("[...truncated: message cap exceeded]")));
	co_return;
}

// ---------------------------------------------------------------------------
// LogCapture.MultiThreadStress
// ---------------------------------------------------------------------------

UNTEST_UNIT_OPTS(Claireon, LogCapture, MultiThreadStress, UNTEST_TIMEOUTMS(30000))
{
	using namespace ClaireonLogCaptureSpec;

	FClaireonLogCapture Capture(ELogVerbosity::Warning);

	std::atomic<int32> SuccessCount{0};
	std::atomic<int32> FailureCount{0};

	TArray<TFuture<void>> Futures;
	Futures.Reserve(kStressThreads);

	for (int32 t = 0; t < kStressThreads; ++t)
	{
		Futures.Add(Async(EAsyncExecution::ThreadPool, [t, &SuccessCount, &FailureCount]()
		{
			for (int32 i = 0; i < kStressIterations; ++i)
			{
				const int32 Tpl = (t + i) % kPayloadTemplateCount;
				if (EmitOneStressMessage(Tpl, i))
				{
					SuccessCount.fetch_add(1, std::memory_order_relaxed);
				}
				else
				{
					FailureCount.fetch_add(1, std::memory_order_relaxed);
				}
			}
		}));
	}

	for (TFuture<void>& F : Futures)
	{
		F.Wait();
	}

	// All UNTEST_ASSERT_* on the test thread, OUTSIDE the lambdas.
	UNTEST_ASSERT_EQ(FailureCount.load(), 0);
	UNTEST_ASSERT_EQ(SuccessCount.load(), kStressThreads * kStressIterations);

	const FString Output = Capture.GetCapturedOutput();
	const bool bTruncated = Output.Contains(TEXT("[...truncated: message cap exceeded]"));

	// Parse line-by-line. For every "[Warning] LogClaireon: " line, validate
	// the payload matches one of the three templates with an integer in
	// [0, kStressIterations). This catches torn FString copies.
	const FString WarningPrefix = TEXT("[Warning] LogClaireon: ");

	TArray<FString> Lines;
	Output.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);

	int32 CapturedClaireonWarnings = 0;
	bool bAllPayloadsValid = true;
	bool bAnyDeniedCategoryLine = false;

	// Default-denylist categories. Any line whose category prefix matches one
	// of these implies the snapshot was bypassed -- a regression.
	const TCHAR* const DeniedCategoryPrefixes[] = {
		TEXT("[Warning] LogBlueprint: "),
		TEXT("[Warning] LogAnimation: "),
		TEXT("[Warning] LogAnimationCompressionInternal: "),
		TEXT("[Warning] LogLinker: "),
		TEXT("[Warning] LogStreaming: "),
		TEXT("[Warning] LogSlate: "),
		TEXT("[Warning] LogChooser: "),
		TEXT("[Error] LogBlueprint: "),
		TEXT("[Error] LogAnimation: "),
		TEXT("[Error] LogAnimationCompressionInternal: "),
		TEXT("[Error] LogLinker: "),
		TEXT("[Error] LogStreaming: "),
		TEXT("[Error] LogSlate: "),
		TEXT("[Error] LogChooser: "),
	};

	for (const FString& Line : Lines)
	{
		if (Line.IsEmpty())
		{
			continue;
		}
		// Skip the truncation sentinel.
		if (Line.StartsWith(TEXT("[...truncated:")))
		{
			continue;
		}

		for (const TCHAR* DeniedPrefix : DeniedCategoryPrefixes)
		{
			if (Line.StartsWith(DeniedPrefix))
			{
				bAnyDeniedCategoryLine = true;
				break;
			}
		}

		if (!Line.StartsWith(WarningPrefix))
		{
			continue;
		}

		++CapturedClaireonWarnings;

		const FString Payload = Line.Mid(WarningPrefix.Len());

		bool bMatchedTemplate = false;
		for (int32 TplIdx = 0; TplIdx < kPayloadTemplateCount; ++TplIdx)
		{
			const FString Stem = kStressTemplateStems[TplIdx];
			if (Payload.StartsWith(Stem))
			{
				const FString IntPart = Payload.Mid(Stem.Len());
				if (IntPart.IsNumeric())
				{
					const int32 ParsedValue = FCString::Atoi(*IntPart);
					if (ParsedValue >= 0 && ParsedValue < kStressIterations)
					{
						bMatchedTemplate = true;
					}
				}
				break;
			}
		}

		if (!bMatchedTemplate)
		{
			bAllPayloadsValid = false;
		}
	}

	UNTEST_ASSERT_FALSE(bAnyDeniedCategoryLine);
	UNTEST_ASSERT_TRUE(bAllPayloadsValid);

	// Cap invariant: if truncation marker is present OR captured count is at
	// least MaxCapturedMessages, allow over-cap behavior; otherwise total
	// captured warnings must be <= the total emitted.
	if (!bTruncated && CapturedClaireonWarnings < FClaireonLogCapture::MaxCapturedMessages)
	{
		UNTEST_ASSERT_TRUE(CapturedClaireonWarnings <= kStressThreads * kStressIterations);
	}

	co_return;
}

#endif // WITH_UNTESTED
