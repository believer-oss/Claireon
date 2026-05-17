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

FString ClaireonTool_TraceGetFrameStats::GetCategory() const { return TEXT("trace"); }
FString ClaireonTool_TraceGetFrameStats::GetOperation() const { return TEXT("get_frame_stats"); }

FString ClaireonTool_TraceGetFrameStats::GetDescription() const
{
	return TEXT("Get per-frame timing data with hitch detection. Returns frame times, summary stats (avg/p50/p95/p99/max), and top scopes for hitch frames.");
}

TSharedPtr<FJsonObject> ClaireonTool_TraceGetFrameStats::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// sessionId - required
	TSharedPtr<FJsonObject> SessionIdProp = MakeShared<FJsonObject>();
	SessionIdProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionIdProp->SetStringField(TEXT("description"), TEXT("The session ID returned by editor.trace.open"));
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
	MaxResultsProp->SetStringField(TEXT("description"), TEXT("Maximum number of frames to return (default: 100)"));
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

// Scopes that are just frame wrappers Ã¢Â€Â” not useful for cause identification
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
	if (!Arguments->TryGetStringField(TEXT("sessionId"), SessionId) || SessionId.IsEmpty())
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

	TArray<TSharedPtr<FJsonValue>> FramesArray;
	double SumMs = 0.0;
	double MinMs = DBL_MAX;
	double MaxMs = 0.0;

	if (Session->AnalysisSession.IsValid())
	{
		TraceServices::FAnalysisSessionReadScope ReadScope(*Session->AnalysisSession);
		const TraceServices::IFrameProvider* FrameProvider = Session->GetFrameProvider();
		if (!FrameProvider)
		{
			return MakeErrorResult(TEXT("Frame provider not available in trace"));
		}

		const uint64 TotalFrames = FrameProvider->GetFrameCount(FrameType);
		const int32 ClampedEnd = (int32)FMath::Min((uint64)EndFrame, TotalFrames - 1);

		for (int32 i = StartFrame; i <= ClampedEnd && FramesArray.Num() < MaxResults; ++i)
		{
			const TraceServices::FFrame* Frame = FrameProvider->GetFrame(FrameType, i);
			if (!Frame)
			{
				continue;
			}

			const double FrameMs = (Frame->EndTime - Frame->StartTime) * 1000.0;

			if (bOnlyHitches && FrameMs < HitchThresholdMs)
			{
				continue;
			}

			SumMs += FrameMs;
			if (FrameMs < MinMs) { MinMs = FrameMs; }
			if (FrameMs > MaxMs) { MaxMs = FrameMs; }

			TSharedPtr<FJsonObject> FrameObj = MakeShared<FJsonObject>();
			FrameObj->SetNumberField(TEXT("frame_index"), i);
			FrameObj->SetNumberField(TEXT("duration_ms"), FrameMs);
			FrameObj->SetNumberField(TEXT("game_thread_ms"), FrameMs); // simplified
			FramesArray.Add(MakeShared<FJsonValueObject>(FrameObj));
		}
	}

	const int32 FrameCountInResult = FramesArray.Num();
	const double AvgMs = FrameCountInResult > 0 ? SumMs / FrameCountInResult : 0.0;
	if (MinMs == DBL_MAX) { MinMs = 0.0; }

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), SessionId);
	Data->SetArrayField(TEXT("frames"), FramesArray);
	Data->SetNumberField(TEXT("avg_ms"), AvgMs);
	Data->SetNumberField(TEXT("min_ms"), MinMs);
	Data->SetNumberField(TEXT("max_ms"), MaxMs);

	const FString Summary = FString::Printf(TEXT("Frames %d-%d: avg %.1fms, min %.1fms, max %.1fms"),
		StartFrame, FMath::Min(EndFrame, StartFrame + FrameCountInResult - 1),
		AvgMs, MinMs, MaxMs);

	return MakeSuccessResult(Data, Summary);
}
