// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TraceGetSessionInfo.h"
#include "ClaireonLog.h"
#include "ClaireonTraceSession.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Threads.h"

FString ClaireonTool_TraceGetSessionInfo::GetCategory() const { return TEXT("trace"); }
FString ClaireonTool_TraceGetSessionInfo::GetOperation() const { return TEXT("get_session_info"); }

FString ClaireonTool_TraceGetSessionInfo::GetDescription() const
{
	return TEXT("Get session metadata: duration, frame counts, thread count, platform metadata, analysis status");
}

TSharedPtr<FJsonObject> ClaireonTool_TraceGetSessionInfo::GetInputSchema() const
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

IClaireonTool::FToolResult ClaireonTool_TraceGetSessionInfo::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.trace.getSessionInfo"));

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
		return MakeErrorResult(TEXT("Analysis session is invalid"));
	}

	TraceServices::FAnalysisSessionReadScope ReadScope(*Session->AnalysisSession);

	const TraceServices::IFrameProvider* FrameProvider = Session->GetFrameProvider();
	const TraceServices::IThreadProvider* ThreadProvider = Session->GetThreadProvider();

	FString Output;
	Output += FString::Printf(TEXT("sessionId: %s\n"), *Session->SessionId);
	Output += FString::Printf(TEXT("filePath: %s\n"), *Session->FilePath);
	Output += FString::Printf(TEXT("durationSeconds: %.3f\n"), Session->AnalysisSession->GetDurationSeconds());
	Output += FString::Printf(TEXT("analysisComplete: %s\n"),
		Session->AnalysisSession->IsAnalysisComplete() ? TEXT("true") : TEXT("false"));

	// Frame counts
	if (FrameProvider)
	{
		const uint64 GameFrameCount = FrameProvider->GetFrameCount(ETraceFrameType::TraceFrameType_Game);
		const uint64 RenderFrameCount = FrameProvider->GetFrameCount(ETraceFrameType::TraceFrameType_Rendering);
		Output += FString::Printf(TEXT("gameFrameCount: %llu\n"), GameFrameCount);
		Output += FString::Printf(TEXT("renderFrameCount: %llu\n"), RenderFrameCount);
	}

	// Thread count
	if (ThreadProvider)
	{
		int32 ThreadCount = 0;
		ThreadProvider->EnumerateThreads([&ThreadCount](const TraceServices::FThreadInfo&)
		{
			ThreadCount++;
		});
		Output += FString::Printf(TEXT("threadCount: %d\n"), ThreadCount);
	}

	// Metadata
	Output += TEXT("\n--- Metadata ---\n");
	Session->AnalysisSession->EnumerateMetadata([&Output](const TraceServices::FTraceSessionMetadata& Metadata)
	{
		switch (Metadata.Type)
		{
		case TraceServices::FTraceSessionMetadata::EType::String:
			Output += FString::Printf(TEXT("%s: %s\n"), *Metadata.Name.ToString(), *Metadata.StringValue);
			break;
		case TraceServices::FTraceSessionMetadata::EType::Int64:
			Output += FString::Printf(TEXT("%s: %lld\n"), *Metadata.Name.ToString(), Metadata.Int64Value);
			break;
		case TraceServices::FTraceSessionMetadata::EType::Double:
			Output += FString::Printf(TEXT("%s: %.3f\n"), *Metadata.Name.ToString(), Metadata.DoubleValue);
			break;
		}
	});

	return MakeSuccessResult(nullptr, Output);
}
