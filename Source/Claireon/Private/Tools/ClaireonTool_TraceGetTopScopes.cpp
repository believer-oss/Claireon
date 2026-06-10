// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TraceGetTopScopes.h"
#include "ClaireonLog.h"
#include "ClaireonTraceSession.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/TimingProfiler.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Containers/Tables.h"

FString ClaireonTool_TraceGetTopScopes::GetCategory() const { return TEXT("trace"); }
FString ClaireonTool_TraceGetTopScopes::GetOperation() const { return TEXT("get_top_scopes"); }

FString ClaireonTool_TraceGetTopScopes::GetDescription() const
{
    return TEXT("Get aggregated top-N CPU scopes by time. The primary hitch investigation tool -- answers 'what is taking the most time?'. Stateless / read-only / non-session: reads from an open trace handle.");
}

TSharedPtr<FJsonObject> ClaireonTool_TraceGetTopScopes::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// sessionId - required
	TSharedPtr<FJsonObject> SessionIdProp = MakeShared<FJsonObject>();
	SessionIdProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionIdProp->SetStringField(TEXT("description"), TEXT("The session ID returned by editor.trace.open"));
	Properties->SetObjectField(TEXT("sessionId"), SessionIdProp);

	// startTime / endTime - optional (time range in seconds)
	TSharedPtr<FJsonObject> StartTimeProp = MakeShared<FJsonObject>();
	StartTimeProp->SetStringField(TEXT("type"), TEXT("number"));
	StartTimeProp->SetStringField(TEXT("description"), TEXT("Start of time range in seconds (alternative to startFrame)"));
	Properties->SetObjectField(TEXT("startTime"), StartTimeProp);

	TSharedPtr<FJsonObject> EndTimeProp = MakeShared<FJsonObject>();
	EndTimeProp->SetStringField(TEXT("type"), TEXT("number"));
	EndTimeProp->SetStringField(TEXT("description"), TEXT("End of time range in seconds (alternative to endFrame)"));
	Properties->SetObjectField(TEXT("endTime"), EndTimeProp);

	// startFrame / endFrame - optional (frame range)
	TSharedPtr<FJsonObject> StartFrameProp = MakeShared<FJsonObject>();
	StartFrameProp->SetStringField(TEXT("type"), TEXT("integer"));
	StartFrameProp->SetStringField(TEXT("description"), TEXT("Start frame index (alternative to startTime)"));
	Properties->SetObjectField(TEXT("startFrame"), StartFrameProp);

	TSharedPtr<FJsonObject> EndFrameProp = MakeShared<FJsonObject>();
	EndFrameProp->SetStringField(TEXT("type"), TEXT("integer"));
	EndFrameProp->SetStringField(TEXT("description"), TEXT("End frame index (alternative to endTime)"));
	Properties->SetObjectField(TEXT("endFrame"), EndFrameProp);

	// threadFilter - optional
	TSharedPtr<FJsonObject> ThreadFilterProp = MakeShared<FJsonObject>();
	ThreadFilterProp->SetStringField(TEXT("type"), TEXT("string"));
	ThreadFilterProp->SetStringField(TEXT("description"),
		TEXT("Filter to specific thread by name substring (e.g. 'GameThread', 'RenderThread')"));
	Properties->SetObjectField(TEXT("threadFilter"), ThreadFilterProp);

	// includeGpu - optional
	TSharedPtr<FJsonObject> IncludeGpuProp = MakeShared<FJsonObject>();
	IncludeGpuProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IncludeGpuProp->SetStringField(TEXT("description"), TEXT("Include GPU timelines in aggregation (default: false)"));
	Properties->SetObjectField(TEXT("includeGpu"), IncludeGpuProp);

	// sortBy - optional
	TSharedPtr<FJsonObject> SortByProp = MakeShared<FJsonObject>();
	SortByProp->SetStringField(TEXT("type"), TEXT("string"));
	SortByProp->SetStringField(TEXT("description"), TEXT("Sort column (default: 'totalInclusive')"));
	{
		TArray<TSharedPtr<FJsonValue>> EnumValues;
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("totalInclusive")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("totalExclusive")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("maxInclusive")));
		EnumValues.Add(MakeShared<FJsonValueString>(TEXT("count")));
		SortByProp->SetArrayField(TEXT("enum"), EnumValues);
	}
	Properties->SetObjectField(TEXT("sortBy"), SortByProp);

	// maxResults - optional
	TSharedPtr<FJsonObject> MaxResultsProp = MakeShared<FJsonObject>();
	MaxResultsProp->SetStringField(TEXT("type"), TEXT("integer"));
	MaxResultsProp->SetStringField(TEXT("description"), TEXT("Maximum number of scopes to return (default: 50)"));
	Properties->SetObjectField(TEXT("maxResults"), MaxResultsProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("sessionId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_TraceGetTopScopes::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	int32 MaxResults = 50;
	FString SortBy = TEXT("totalInclusive");
	FString ThreadFilter;
	bool bIncludeGpu = false;

	Arguments->TryGetNumberField(TEXT("maxResults"), MaxResults);
	Arguments->TryGetStringField(TEXT("sortBy"), SortBy);
	Arguments->TryGetStringField(TEXT("threadFilter"), ThreadFilter);
	Arguments->TryGetBoolField(TEXT("includeGpu"), bIncludeGpu);

	// Aggregate timing data from the timing profiler provider
	struct FScopeEntry
	{
		FString Name;
		double TotalMs = 0.0;
		double AvgMs = 0.0;
		uint64 CallCount = 0;
	};

	TArray<FScopeEntry> Scopes;

	if (Session->AnalysisSession.IsValid())
	{
		TraceServices::FAnalysisSessionReadScope ReadScope(*Session->AnalysisSession);
		const TraceServices::ITimingProfilerProvider* TimingProvider = Session->GetTimingProvider();
		const TraceServices::IThreadProvider* ThreadProvider = Session->GetThreadProvider();
		const TraceServices::IFrameProvider* FrameProvider = Session->GetFrameProvider();

		if (TimingProvider && ThreadProvider)
		{
			// Determine time range
			double IntervalStart = 0.0;
			double IntervalEnd = Session->AnalysisSession->GetDurationSeconds();

			double StartTimeSec = 0.0, EndTimeSec = 0.0;
			bool bHasStartTime = Arguments->TryGetNumberField(TEXT("startTime"), StartTimeSec);
			bool bHasEndTime = Arguments->TryGetNumberField(TEXT("endTime"), EndTimeSec);
			if (bHasStartTime) IntervalStart = StartTimeSec;
			if (bHasEndTime) IntervalEnd = EndTimeSec;

			// Frame params override time params
			int32 StartFrame = -1, EndFrame = -1;
			Arguments->TryGetNumberField(TEXT("startFrame"), StartFrame);
			Arguments->TryGetNumberField(TEXT("endFrame"), EndFrame);
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

			// Build thread ID -> name map for optional filtering
			TMap<uint32, FString> ThreadNames;
			if (!ThreadFilter.IsEmpty())
			{
				ThreadProvider->EnumerateThreads([&](const TraceServices::FThreadInfo& ThreadInfo)
				{
					ThreadNames.Add(ThreadInfo.Id, FString(ThreadInfo.Name ? ThreadInfo.Name : TEXT("")));
				});
			}

			// Run aggregation over the time range
			TraceServices::FCreateAggreationParams Params;
			Params.IntervalStart = IntervalStart;
			Params.IntervalEnd = IntervalEnd;
			Params.IncludeGpu = bIncludeGpu;

			if (!ThreadFilter.IsEmpty())
			{
				Params.CpuThreadFilter = [ThreadNames, ThreadFilter](uint32 ThreadId) -> bool
				{
					const FString* Name = ThreadNames.Find(ThreadId);
					if (!Name) return false;
					return Name->Contains(ThreadFilter, ESearchCase::IgnoreCase);
				};
			}
			else
			{
				Params.CpuThreadFilter = [](uint32) -> bool { return true; };
			}

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
							FScopeEntry Entry;
							Entry.Name = Row->Timer->Name;
							Entry.TotalMs = Row->TotalInclusiveTime * 1000.0;
							Entry.AvgMs = Row->AverageInclusiveTime * 1000.0;
							Entry.CallCount = Row->InstanceCount;
							Scopes.Add(Entry);
						}
						Reader->NextRow();
					}
					delete Reader;
				}
				delete AggTable;
			}
		}
	}

	// Sort
	if (SortBy == TEXT("count"))
	{
		Scopes.Sort([](const FScopeEntry& A, const FScopeEntry& B) { return A.CallCount > B.CallCount; });
	}
	else // totalInclusive / total_time (default)
	{
		Scopes.Sort([](const FScopeEntry& A, const FScopeEntry& B) { return A.TotalMs > B.TotalMs; });
	}

	if (Scopes.Num() > MaxResults)
	{
		Scopes.SetNum(MaxResults);
	}

	// Build output
	TArray<TSharedPtr<FJsonValue>> ScopesArray;
	for (const FScopeEntry& Scope : Scopes)
	{
		TSharedPtr<FJsonObject> ScopeObj = MakeShared<FJsonObject>();
		ScopeObj->SetStringField(TEXT("name"), Scope.Name);
		ScopeObj->SetNumberField(TEXT("total_ms"), Scope.TotalMs);
		ScopeObj->SetNumberField(TEXT("avg_ms"), Scope.AvgMs);
		ScopeObj->SetNumberField(TEXT("call_count"), (double)Scope.CallCount);
		ScopesArray.Add(MakeShared<FJsonValueObject>(ScopeObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), SessionId);
	Data->SetArrayField(TEXT("scopes"), ScopesArray);

	FString TopScopeName = ScopesArray.Num() > 0
		? ScopesArray[0]->AsObject()->GetStringField(TEXT("name"))
		: TEXT("none");
	double TopAvgMs = ScopesArray.Num() > 0
		? ScopesArray[0]->AsObject()->GetNumberField(TEXT("avg_ms"))
		: 0.0;

	const FString Summary = FString::Printf(TEXT("Top %d scopes: %s (%.1fms avg)"),
		ScopesArray.Num(), *TopScopeName, TopAvgMs);

	return MakeSuccessResult(Data, Summary);
}
