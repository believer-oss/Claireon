// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TraceGetThreads.h"
#include "ClaireonLog.h"
#include "ClaireonTraceSession.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Threads.h"

FString ClaireonTool_TraceGetThreads::GetCategory() const { return TEXT("trace"); }
FString ClaireonTool_TraceGetThreads::GetOperation() const { return TEXT("get_threads"); }

FString ClaireonTool_TraceGetThreads::GetDescription() const
{
    return TEXT("List all threads in the trace with their names and groups. Stateless / read-only / non-session: reads from an open trace handle without opening any asset.");
}

TSharedPtr<FJsonObject> ClaireonTool_TraceGetThreads::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> SessionIdProp = MakeShared<FJsonObject>();
	SessionIdProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionIdProp->SetStringField(TEXT("description"), TEXT("The session ID returned by trace_open"));
	Properties->SetObjectField(TEXT("sessionId"), SessionIdProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("sessionId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_TraceGetThreads::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Display, TEXT("[MCP] trace_get_threads"));

	FString SessionId;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("sessionId"), SessionId))
	{
		return MakeErrorResult(TEXT("Missing required parameter: sessionId"));
	}

	FClaireonTraceSession* Session = FClaireonTraceSessionManager::Get().FindSession(SessionId);
	if (!Session)
	{
		return MakeErrorResult(FString::Printf(TEXT("Session not found or expired: %s"), *SessionId));
	}

	if (!Session->AnalysisSession.IsValid())
	{
		return MakeErrorResult(TEXT("Analysis session is not available"));
	}

	TraceServices::FAnalysisSessionReadScope ReadScope(*Session->AnalysisSession);

	const TraceServices::IThreadProvider* ThreadProvider = Session->GetThreadProvider();
	if (!ThreadProvider)
	{
		return MakeErrorResult(TEXT("Thread provider is not available"));
	}

	// P0-6d: this tool used to return MakeSuccessResult(nullptr, <prose dump>).
	// A null Data becomes {} in the Python envelope, so the obvious
	// result["data"]["threads"] was a KeyError and the only way to get at the
	// thread list was to re-parse the human summary. Data is now the primary
	// channel; the summary is reduced to one line.
	TArray<TSharedPtr<FJsonValue>> ThreadsArray;

	ThreadProvider->EnumerateThreads([&ThreadsArray](const TraceServices::FThreadInfo& ThreadInfo)
	{
		TSharedPtr<FJsonObject> ThreadObj = MakeShared<FJsonObject>();
		ThreadObj->SetNumberField(TEXT("id"), static_cast<double>(ThreadInfo.Id));
		ThreadObj->SetStringField(TEXT("name"),
			ThreadInfo.Name ? ThreadInfo.Name : TEXT("(unnamed)"));
		ThreadObj->SetStringField(TEXT("group_name"),
			ThreadInfo.GroupName ? ThreadInfo.GroupName : TEXT("(none)"));
		ThreadsArray.Add(MakeShared<FJsonValueObject>(ThreadObj));
	});

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), SessionId);
	Data->SetArrayField(TEXT("threads"), ThreadsArray);
	// Derived from the emitted array, not from a provider total, so the count
	// can never disagree with what the caller actually received.
	Data->SetNumberField(TEXT("thread_count"), ThreadsArray.Num());

	const FString Summary = FString::Printf(TEXT("%d thread(s) in trace"), ThreadsArray.Num());

	return MakeSuccessResult(Data, Summary);
}
