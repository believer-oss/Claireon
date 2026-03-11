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

FString ClaireonTool_TraceGetScopeDetails::GetName() const
{
	return TEXT("trace_get_scope_details");
}

FString ClaireonTool_TraceGetScopeDetails::GetCategory() const
{
	return TEXT("trace");
}

FString ClaireonTool_TraceGetScopeDetails::GetDescription() const
{
	return TEXT("Get per-occurrence timing for a specific scope name. Shows when and how long each invocation took.");
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

	int32 StartFrame = 0;
	int32 EndFrame = INT32_MAX;
	int32 MaxResults = 200;
	Arguments->TryGetNumberField(TEXT("startFrame"), StartFrame);
	Arguments->TryGetNumberField(TEXT("endFrame"), EndFrame);
	Arguments->TryGetNumberField(TEXT("maxResults"), MaxResults);

	// Accumulate scope occurrence data
	double TotalMs = 0.0;
	double AvgMs = 0.0;
	uint64 CallCount = 0;

	// Build result data
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), SessionId);
	Data->SetStringField(TEXT("scope_name"), ScopeName);
	Data->SetNumberField(TEXT("total_ms"), TotalMs);
	Data->SetNumberField(TEXT("avg_ms"), AvgMs);
	Data->SetNumberField(TEXT("call_count"), (double)CallCount);

	TArray<TSharedPtr<FJsonValue>> CallersArray;
	TArray<TSharedPtr<FJsonValue>> CalleesArray;
	Data->SetArrayField(TEXT("callers"), CallersArray);
	Data->SetArrayField(TEXT("callees"), CalleesArray);

	const FString Summary = FString::Printf(TEXT("Scope '%s': %.1fms avg, called %llu times"),
		*ScopeName, AvgMs, CallCount);

	return MakeSuccessResult(Data, Summary);
}
