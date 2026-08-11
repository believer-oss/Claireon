// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TraceGetFrameStats.h"
#include "ClaireonLog.h"
#include "ClaireonTraceSession.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/TimingProfiler.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Containers/Tables.h"

// ---------------------------------------------------------------------------
// P0-6a: frame aggregation, extracted so it is reachable without a capture.
//
// The engine seeds every open frame with EndTime = +inf
// (TraceServices/Private/Model/Frames.cpp), so a capture stopped mid-frame
// yields a non-finite duration. Fed into the running sum unguarded, that
// poisoned avg_ms completely and max_ms whenever the value was +inf -- the
// realistic case. min_ms was never affected, since +inf never wins a minimum.
//
// The result was a confident wrong number rather than a visible failure, which
// is why the excluded frames are now counted and named instead of dropped.
//
// External linkage so the unit tests can drive it with a hand-built
// {16.6, inf, 16.7} array; a capture cannot be made to contain an open frame
// on demand. Mirrors the ClaireonLogSearchEncoding test seam.
// ---------------------------------------------------------------------------
namespace ClaireonTraceFrameStats
{

FFrameAggregate AggregateFrameSamples(const TArray<FFrameSample>& Samples)
{
	FFrameAggregate Out;

	double SumMs = 0.0;
	double MinMs = DBL_MAX;
	// Matches the pre-extraction initial value so an empty range still reports 0.
	double MaxMs = 0.0;

	for (const FFrameSample& Sample : Samples)
	{
		if (!FMath::IsFinite(Sample.DurationMs))
		{
			Out.UnterminatedFrameIndices.Add(Sample.FrameIndex);
			continue;
		}

		SumMs += Sample.DurationMs;
		if (Sample.DurationMs < MinMs) { MinMs = Sample.DurationMs; }
		if (Sample.DurationMs > MaxMs) { MaxMs = Sample.DurationMs; }
		++Out.FiniteFrameCount;
	}

	Out.AvgMs = Out.FiniteFrameCount > 0 ? SumMs / Out.FiniteFrameCount : 0.0;
	Out.MinMs = (MinMs == DBL_MAX) ? 0.0 : MinMs;
	Out.MaxMs = MaxMs;
	return Out;
}

} // namespace ClaireonTraceFrameStats

FString ClaireonTool_TraceGetFrameStats::GetCategory() const { return TEXT("trace"); }
FString ClaireonTool_TraceGetFrameStats::GetOperation() const { return TEXT("get_frame_stats"); }

FString ClaireonTool_TraceGetFrameStats::GetDescription() const
{
    // C4 hardening: the previous text advertised hitch detection with p50/p95/p99
    // percentiles and "top scopes for hitch frames" -- none of that is computed.
    // What IS real: per-frame duration_ms, and an onlyHitches/hitchThresholdMs
    // filter over which frames are collected in the first place.
    // Kept under the 400-char P5 description cap that DescriptionLint enforces; the
    // per-field caveats are repeated on the individual schema fields.
    return TEXT("Get per-frame timing data, optionally filtered to frames over hitchThresholdMs via "
                "onlyHitches. Returns each frame's duration_ms plus avg_ms/min_ms/max_ms computed only over "
                "the frames actually returned after maxResults truncation, not the full range, and not "
                "percentiles. game_thread_ms always equals duration_ms. Does not identify which scopes caused "
                "a hitch. Stateless, read-only, non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_TraceGetFrameStats::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// sessionId - required
	TSharedPtr<FJsonObject> SessionIdProp = MakeShared<FJsonObject>();
	SessionIdProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionIdProp->SetStringField(TEXT("description"), TEXT("The session ID returned by trace_open"));
	Properties->SetObjectField(TEXT("sessionId"), SessionIdProp);

	// frameType - optional
	TSharedPtr<FJsonObject> FrameTypeProp = MakeShared<FJsonObject>();
	FrameTypeProp->SetStringField(TEXT("type"), TEXT("string"));
	FrameTypeProp->SetStringField(TEXT("description"), TEXT("Frame type to analyze (default: 'game')"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("game")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("render")));
		FrameTypeProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("frameType"), FrameTypeProp);

	// startFrame - optional
	TSharedPtr<FJsonObject> StartFrameProp = MakeShared<FJsonObject>();
	StartFrameProp->SetStringField(TEXT("type"), TEXT("integer"));
	StartFrameProp->SetStringField(TEXT("description"), TEXT("Start frame index (default: 0)"));
	Properties->SetObjectField(TEXT("startFrame"), StartFrameProp);

	// endFrame - optional
	TSharedPtr<FJsonObject> EndFrameProp = MakeShared<FJsonObject>();
	EndFrameProp->SetStringField(TEXT("type"), TEXT("integer"));
	EndFrameProp->SetStringField(TEXT("description"), TEXT("End frame index (default: last frame)"));
	Properties->SetObjectField(TEXT("endFrame"), EndFrameProp);

	// hitchThresholdMs - optional
	TSharedPtr<FJsonObject> HitchProp = MakeShared<FJsonObject>();
	HitchProp->SetStringField(TEXT("type"), TEXT("number"));
	HitchProp->SetStringField(TEXT("description"), TEXT("Frame time threshold in ms to count as a hitch (default: 33.3)"));
	Properties->SetObjectField(TEXT("hitchThresholdMs"), HitchProp);

	// onlyHitches - optional
	TSharedPtr<FJsonObject> OnlyHitchesProp = MakeShared<FJsonObject>();
	OnlyHitchesProp->SetStringField(TEXT("type"), TEXT("boolean"));
	OnlyHitchesProp->SetStringField(TEXT("description"), TEXT("If true, only return frames exceeding the hitch threshold (default: false)"));
	Properties->SetObjectField(TEXT("onlyHitches"), OnlyHitchesProp);

	// maxResults - optional
	TSharedPtr<FJsonObject> MaxResultsProp = MakeShared<FJsonObject>();
	MaxResultsProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxResultsProp->SetStringField(TEXT("description"),
		TEXT("Maximum number of frames to return (default: 100). The frame scan STOPS as soon as this many ")
		TEXT("have been collected, so avg_ms/min_ms/max_ms in the response describe only the returned page, ")
		TEXT("not the full startFrame-endFrame range -- a hitch further into a large range than maxResults ")
		TEXT("allows will not be seen at all."));
	Properties->SetObjectField(TEXT("maxResults"), MaxResultsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("sessionId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

// Shared scope entry used by hitch analysis
struct FHitchScopeEntry
{
	FString Name;
	FString File;
	uint32 Line = 0;
	double TotalInclusiveMs = 0.0;
	double TotalExclusiveMs = 0.0;
	uint64 Count = 0;
};

// Scopes that are just frame wrappers - not useful for cause identification
static const TSet<FString> GFrameWrapperScopes = {
	TEXT("Frame"),
	TEXT("FEngineLoop::Tick"),
};

namespace ClaireonTool_TraceGetFrameStatsInternal
{

// Read aggregated scopes from a table, filtering out zero-instance entries
TArray<FHitchScopeEntry> ReadScopesFromAggregation(
	TraceServices::ITable<TraceServices::FTimingProfilerAggregatedStats>* AggTable)
{
	TArray<FHitchScopeEntry> Result;
	if (!AggTable)
	{
		return Result;
	}

	TraceServices::ITableReader<TraceServices::FTimingProfilerAggregatedStats>* Reader = AggTable->CreateReader();
	if (Reader)
	{
		while (Reader->IsValid())
		{
			const TraceServices::FTimingProfilerAggregatedStats* Row = Reader->GetCurrentRow();
			if (Row && Row->Timer && Row->Timer->Name && Row->InstanceCount > 0)
			{
				FHitchScopeEntry Entry;
				Entry.Name = Row->Timer->Name;
				Entry.File = Row->Timer->File ? FString(Row->Timer->File) : TEXT("");
				Entry.Line = Row->Timer->Line;
				Entry.TotalInclusiveMs = Row->TotalInclusiveTime * 1000.0;
				Entry.TotalExclusiveMs = Row->TotalExclusiveTime * 1000.0;
				Entry.Count = Row->InstanceCount;
				Result.Add(Entry);
			}
			Reader->NextRow();
		}
		delete Reader;
	}

	delete AggTable;
	return Result;
}

}  // namespace ClaireonTool_TraceGetFrameStatsInternal

IClaireonTool::FToolResult ClaireonTool_TraceGetFrameStats::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("sessionId"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: sessionId"));
	}

	FClaireonTraceSession* Session = FClaireonTraceSessionManager::Get().FindSession(SessionId);
	if (!Session)
	{
		return MakeErrorResult(FString::Printf(TEXT("Trace session not found: %s"), *SessionId));
	}
	Session->Touch();

	FString FrameTypeStr = TEXT("game");
	Arguments->TryGetStringField(TEXT("frameType"), FrameTypeStr);
	const ETraceFrameType FrameType = (FrameTypeStr == TEXT("render"))
		? TraceFrameType_Rendering
		: TraceFrameType_Game;

	int32 StartFrame = 0;
	int32 EndFrame = INT32_MAX;
	int32 MaxResults = 100;
	double HitchThresholdMs = 33.3;
	bool bOnlyHitches = false;

	Arguments->TryGetNumberField(TEXT("startFrame"), StartFrame);
	Arguments->TryGetNumberField(TEXT("endFrame"), EndFrame);
	Arguments->TryGetNumberField(TEXT("maxResults"), MaxResults);
	Arguments->TryGetNumberField(TEXT("hitchThresholdMs"), HitchThresholdMs);
	Arguments->TryGetBoolField(TEXT("onlyHitches"), bOnlyHitches);

	TArray<ClaireonTraceFrameStats::FFrameSample> Samples;

	if (Session->AnalysisSession.IsValid())
	{
		TraceServices::FAnalysisSessionReadScope ReadScope(*Session->AnalysisSession);
		const TraceServices::IFrameProvider* FrameProvider = Session->GetFrameProvider();
		if (!FrameProvider)
		{
			return MakeErrorResult(TEXT("Frame provider not available in trace"));
		}

		const uint64 TotalFrames = FrameProvider->GetFrameCount(FrameType);

		// Defect guard: TotalFrames is uint64, so "TotalFrames - 1" UNDERFLOWS to
		// UINT64_MAX when the trace holds no frames of this type -- reachable without
		// contrivance via frameType="render" on a trace with no rendering frames. The
		// Min then kept EndFrame's default of INT32_MAX and the loop below made ~2.1
		// billion GetFrame calls on the GAME THREAD, hanging the editor. The null-frame
		// path is `continue`, not `break`, so it burned every iteration.
		//
		// StartFrame is also read straight from arguments with no validation, so clamp
		// it to 0 rather than trusting the caller not to send a negative index.
		const int32 ClampedStart = FMath::Max(0, StartFrame);
		const int32 ClampedEnd = (TotalFrames == 0)
			? -1 // empty range: the loop cannot execute
			: (int32)FMath::Min<uint64>((uint64)EndFrame, TotalFrames - 1);

		for (int32 i = ClampedStart; i <= ClampedEnd && Samples.Num() < MaxResults; ++i)
		{
			const TraceServices::FFrame* Frame = FrameProvider->GetFrame(FrameType, i);
			if (!Frame)
			{
				continue;
			}

			const double FrameMs = (Frame->EndTime - Frame->StartTime) * 1000.0;

			// An unterminated frame's duration is non-finite. Guard the hitch
			// comparison explicitly: every ordering test against NaN is false, so
			// an unguarded `FrameMs < Threshold` would silently keep NaN frames and
			// drop nothing, while +inf would always read as a hitch. Non-finite
			// frames are kept as samples so they can be disclosed downstream, never
			// filtered on a meaningless comparison.
			if (bOnlyHitches && FMath::IsFinite(FrameMs) && FrameMs < HitchThresholdMs)
			{
				continue;
			}

			Samples.Add(ClaireonTraceFrameStats::FFrameSample{ i, FrameMs });
		}
	}

	const ClaireonTraceFrameStats::FFrameAggregate Aggregate =
		ClaireonTraceFrameStats::AggregateFrameSamples(Samples);

	TArray<TSharedPtr<FJsonValue>> FramesArray;
	FramesArray.Reserve(Samples.Num());
	for (const ClaireonTraceFrameStats::FFrameSample& Sample : Samples)
	{
		TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
		FrameObj->SetNumberField(TEXT("frame_index"), Sample.FrameIndex);

		if (FMath::IsFinite(Sample.DurationMs))
		{
			FrameObj->SetNumberField(TEXT("duration_ms"), Sample.DurationMs);
			// C4 hardening note: this is NOT a distinct game-thread-only measurement --
			// it is a placeholder that always equals duration_ms above. A real
			// game-thread-vs-other-thread breakdown per frame is not implemented. Kept
			// (rather than dropped) for wire-format stability; see GetDescription().
			FrameObj->SetNumberField(TEXT("game_thread_ms"), Sample.DurationMs);
		}
		else
		{
			// Null rather than 0.0: a zero is just another plausible wrong number,
			// and the flag says why it is missing instead of leaving the caller to
			// infer it.
			FrameObj->SetField(TEXT("duration_ms"), MakeShared<FJsonValueNull>());
			FrameObj->SetField(TEXT("game_thread_ms"), MakeShared<FJsonValueNull>());
			FrameObj->SetBoolField(TEXT("unterminated"), true);
		}

		FramesArray.Add(MakeShared<FJsonValueObject>(FrameObj));
	}

	const int32 FrameCountInResult = Samples.Num();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), SessionId);
	Data->SetArrayField(TEXT("frames"), FramesArray);
	Data->SetNumberField(TEXT("avg_ms"), Aggregate.AvgMs);
	Data->SetNumberField(TEXT("min_ms"), Aggregate.MinMs);
	Data->SetNumberField(TEXT("max_ms"), Aggregate.MaxMs);
	Data->SetNumberField(TEXT("unterminated_frames"), Aggregate.UnterminatedFrameIndices.Num());

	const FString Summary = FString::Printf(TEXT("Frames %d-%d: avg %.1fms, min %.1fms, max %.1fms"),
		StartFrame, FMath::Min(EndFrame, StartFrame + FrameCountInResult - 1),
		Aggregate.AvgMs, Aggregate.MinMs, Aggregate.MaxMs);

	FToolResult Result = MakeSuccessResult(Data, Summary);

	// Name the excluded frames. A bare count would leave the caller unable to
	// tell whether the aggregate it just read is trustworthy.
	if (Aggregate.UnterminatedFrameIndices.Num() > 0)
	{
		TArray<FString> IndexStrings;
		IndexStrings.Reserve(Aggregate.UnterminatedFrameIndices.Num());
		for (const int32 FrameIndex : Aggregate.UnterminatedFrameIndices)
		{
			IndexStrings.Add(FString::FromInt(FrameIndex));
		}
		Result.Warnings.Add(FString::Printf(
			TEXT("%d unterminated frame(s) excluded from avg_ms/min_ms/max_ms (frame_index: %s). ")
			TEXT("These frames were still open when the capture stopped."),
			Aggregate.UnterminatedFrameIndices.Num(), *FString::Join(IndexStrings, TEXT(", "))));
	}

	return Result;
}
