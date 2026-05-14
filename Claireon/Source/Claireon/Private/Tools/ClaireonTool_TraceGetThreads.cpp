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
	return TEXT("List all threads in the trace with their names and groups");
}

TSharedPtr<FJsonObject> ClaireonTool_TraceGetThreads::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> SessionIdProp = MakeShared<FJsonObject>();
	SessionIdProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionIdProp->SetStringField(TEXT("description"), TEXT("The session ID returned by editor.trace.open"));
	Properties->SetObjectField(TEXT("sessionId"), SessionIdProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("sessionId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_TraceGetThreads::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.trace.getThreads"));

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

	FString Output;
	int32 Count = 0;

	ThreadProvider->EnumerateThreads([&Output, &Count](const TraceServices::FThreadInfo& ThreadInfo)
	{
		Output += FString::Printf(TEXT("thread[%d].id: %u\n"), Count, ThreadInfo.Id);
		Output += FString::Printf(TEXT("thread[%d].name: %s\n"), Count,
			ThreadInfo.Name ? ThreadInfo.Name : TEXT("(unnamed)"));
		Output += FString::Printf(TEXT("thread[%d].groupName: %s\n"), Count,
			ThreadInfo.GroupName ? ThreadInfo.GroupName : TEXT("(none)"));
		Count++;
	});

	Output = FString::Printf(TEXT("threadCount: %d\n"), Count) + Output;

	return MakeSuccessResult(nullptr, Output);
}
