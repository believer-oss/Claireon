// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TraceGetScopeDetails.h"
#include "ClaireonLog.h"
#include "ClaireonTraceSession.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/TimingProfiler.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Containers/Timelines.h"

FString ClaireonTool_TraceGetScopeDetails::GetCategory() const { return TEXT("trace"); }
FString ClaireonTool_TraceGetScopeDetails::GetOperation() const { return TEXT("get_scope_details"); }

FString ClaireonTool_TraceGetScopeDetails::GetDescription() const
{
    return TEXT("Get per-occurrence timing for a specific scope name. Shows when and how long each invocation took. Stateless / read-only / non-session: reads from an open trace handle.");
}

TSharedPtr<FJsonObject> ClaireonTool_TraceGetScopeDetails::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// sessionId - required
	TSharedPtr<FJsonObject> SessionIdProp = MakeShared<FJsonObject>();
	SessionIdProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionIdProp->SetStringField(TEXT("description"), TEXT("The session ID returned by editor.trace.open"));
	Properties->SetObjectField(TEXT("sessionId"), SessionIdProp);

	// scopeName - required
	TSharedPtr<FJsonObject> ScopeNameProp = MakeShared<FJsonObject>();
	ScopeNameProp->SetStringField(TEXT("type"), TEXT("string"));
	ScopeNameProp->SetStringField(TEXT("description"),
		TEXT("Scope/timer name to search for (substring match against timer names)"));
	Properties->SetObjectField(TEXT("scopeName"), ScopeNameProp);

	// startFrame - optional
	TSharedPtr<FJsonObject> StartFrameProp = MakeShared<FJsonObject>();
	StartFrameProp->SetStringField(TEXT("type"), TEXT("integer"));
	StartFrameProp->SetStringField(TEXT("description"), TEXT("Start frame index (optional)"));
	Properties->SetObjectField(TEXT("startFrame"), StartFrameProp);

	// endFrame - optional
	TSharedPtr<FJsonObject> EndFrameProp = MakeShared<FJsonObject>();
	EndFrameProp->SetStringField(TEXT("type"), TEXT("integer"));
	EndFrameProp->SetStringField(TEXT("description"), TEXT("End frame index (optional)"));
	Properties->SetObjectField(TEXT("endFrame"), EndFrameProp);

	// maxResults - optional
	TSharedPtr<FJsonObject> MaxResultsProp = MakeShared<FJsonObject>();
	MaxResultsProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxResultsProp->SetStringField(TEXT("description"), TEXT("Maximum number of occurrences to return (default: 200)"));
	Properties->SetObjectField(TEXT("maxResults"), MaxResultsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("sessionId")));
	Required.Add(MakeShared<FJsonValueString>(TEXT("scopeName")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_TraceGetScopeDetails::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("sessionId"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: sessionId"));
	}

	FString ScopeName;
	if (!Arguments->TryGetStringField(TEXT("scopeName"), ScopeName) || ScopeName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: scopeName"));
	}

	FClaireonTraceSession* Session = FClaireonTraceSessionManager::Get().FindSession(SessionId);
	if (!Session)
	{
		return MakeErrorResult(FString::Printf(TEXT("Trace session not found: %s"), *SessionId));
	}
	Session->Touch();

	int32 StartFrame = -1;
	int32 EndFrame = -1;
	int32 MaxResults = 200;
	Arguments->TryGetNumberField(TEXT("startFrame"), StartFrame);
	Arguments->TryGetNumberField(TEXT("endFrame"), EndFrame);
	Arguments->TryGetNumberField(TEXT("maxResults"), MaxResults);

	// Aggregated stats for scopes matching the name
	struct FMatchedScope
	{
		FString Name;
		double TotalMs = 0.0;
		double AvgMs = 0.0;
		double MaxMs = 0.0;
		uint64 CallCount = 0;
	};
	TArray<FMatchedScope> Matches;

	if (Session->AnalysisSession.IsValid())
	{
		TraceServices::FAnalysisSessionReadScope ReadScope(*Session->AnalysisSession);
		const TraceServices::ITimingProfilerProvider* TimingProvider = Session->GetTimingProvider();
		const TraceServices::IFrameProvider* FrameProvider = Session->GetFrameProvider();

		if (TimingProvider)
		{
			// Determine time range
			double IntervalStart = 0.0;
			double IntervalEnd = Session->AnalysisSession->GetDurationSeconds();

			if (FrameProvider && StartFrame >= 0)
			{
				const TraceServices::FFrame* Frame = FrameProvider->GetFrame(TraceFrameType_Game, StartFrame);
				if (Frame) IntervalStart = Frame->StartTime;
			}
			if (FrameProvider && EndFrame >= 0)
			{
				const TraceServices::FFrame* Frame = FrameProvider->GetFrame(TraceFrameType_Game, EndFrame);
				if (Frame) IntervalEnd = Frame->EndTime;
			}

			TraceServices::FCreateAggreationParams Params;
			Params.IntervalStart = IntervalStart;
			Params.IntervalEnd = IntervalEnd;
			Params.IncludeGpu = false;
			Params.CpuThreadFilter = [](uint32) -> bool { return true; };

			TraceServices::ITable<TraceServices::FTimingProfilerAggregatedStats>* AggTable = TimingProvider->CreateAggregation(Params);
			if (AggTable)
			{
				TraceServices::ITableReader<TraceServices::FTimingProfilerAggregatedStats>* Reader = AggTable->CreateReader();
				if (Reader)
				{
					while (Reader->IsValid())
					{
						const TraceServices::FTimingProfilerAggregatedStats* Row = Reader->GetCurrentRow();
						if (Row && Row->Timer && Row->Timer->Name && Row->InstanceCount > 0)
						{
							const FString TimerName(Row->Timer->Name);
							if (TimerName.Contains(ScopeName, ESearchCase::IgnoreCase))
							{
								FMatchedScope Entry;
								Entry.Name = TimerName;
								Entry.TotalMs = Row->TotalInclusiveTime * 1000.0;
								Entry.AvgMs = Row->AverageInclusiveTime * 1000.0;
								Entry.MaxMs = Row->MaxInclusiveTime * 1000.0;
								Entry.CallCount = Row->InstanceCount;
								Matches.Add(Entry);
							}
						}
						Reader->NextRow();
					}
					delete Reader;
				}
				delete AggTable;
			}
		}
	}

	Matches.Sort([](const FMatchedScope& A, const FMatchedScope& B) { return A.TotalMs > B.TotalMs; });
	if (Matches.Num() > MaxResults) Matches.SetNum(MaxResults);

	// Aggregate totals across all matches for the summary
	double TotalMs = 0.0;
	double AvgMs = 0.0;
	uint64 CallCount = 0;
	for (const FMatchedScope& M : Matches)
	{
		TotalMs += M.TotalMs;
		CallCount += M.CallCount;
	}
	AvgMs = CallCount > 0 ? TotalMs / (double)CallCount : 0.0;

	TArray<TSharedPtr<FJsonValue>> MatchesArray;
	for (const FMatchedScope& M : Matches)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("name"), M.Name);
		Obj->SetNumberField(TEXT("total_ms"), M.TotalMs);
		Obj->SetNumberField(TEXT("avg_ms"), M.AvgMs);
		Obj->SetNumberField(TEXT("max_ms"), M.MaxMs);
		Obj->SetNumberField(TEXT("call_count"), (double)M.CallCount);
		MatchesArray.Add(MakeShared<FJsonValueObject>(Obj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), SessionId);
	Data->SetStringField(TEXT("scope_name"), ScopeName);
	Data->SetNumberField(TEXT("total_ms"), TotalMs);
	Data->SetNumberField(TEXT("avg_ms"), AvgMs);
	Data->SetNumberField(TEXT("call_count"), (double)CallCount);
	Data->SetArrayField(TEXT("matches"), MatchesArray);
	Data->SetArrayField(TEXT("callers"), TArray<TSharedPtr<FJsonValue>>());
	Data->SetArrayField(TEXT("callees"), TArray<TSharedPtr<FJsonValue>>());

	const FString Summary = FString::Printf(TEXT("Scope '%s': %.1fms avg, called %llu times (%d timer(s) matched)"),
		*ScopeName, AvgMs, CallCount, Matches.Num());

	return MakeSuccessResult(Data, Summary);
}
