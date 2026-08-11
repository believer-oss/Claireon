// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TraceGetSessionInfo.h"
#include "ClaireonLog.h"
#include "ClaireonTraceCaptureManifest.h"
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
	SessionIdProp->SetStringField(TEXT("description"), TEXT("The session ID returned by trace_open"));
	Properties->SetObjectField(TEXT("sessionId"), SessionIdProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("sessionId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_TraceGetSessionInfo::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Display, TEXT("[MCP] trace_get_session_info"));

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

	// P0-6d: this tool used to return MakeSuccessResult(nullptr, <prose dump>).
	// A null Data becomes {} in the Python envelope, so every field below was
	// reachable only by re-parsing a human-formatted string. Data is now the
	// primary channel and the summary is reduced to one line.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), Session->SessionId);
	Data->SetStringField(TEXT("file_path"), Session->FilePath);
	Data->SetNumberField(TEXT("duration_seconds"), Session->AnalysisSession->GetDurationSeconds());
	Data->SetBoolField(TEXT("analysis_complete"), Session->AnalysisSession->IsAnalysisComplete());

	// Frame counts
	if (FrameProvider)
	{
		Data->SetNumberField(TEXT("game_frame_count"),
			static_cast<double>(FrameProvider->GetFrameCount(ETraceFrameType::TraceFrameType_Game)));
		Data->SetNumberField(TEXT("render_frame_count"),
			static_cast<double>(FrameProvider->GetFrameCount(ETraceFrameType::TraceFrameType_Rendering)));
	}

	// Thread count
	if (ThreadProvider)
	{
		int32 ThreadCount = 0;
		ThreadProvider->EnumerateThreads([&ThreadCount](const TraceServices::FThreadInfo&)
		{
			ThreadCount++;
		});
		Data->SetNumberField(TEXT("thread_count"), ThreadCount);
	}

	// Metadata, as an object rather than a formatted block.
	TSharedPtr<FJsonObject> MetadataObj = MakeShared<FJsonObject>();
	Session->AnalysisSession->EnumerateMetadata([&MetadataObj](const TraceServices::FTraceSessionMetadata& Metadata)
	{
		const FString Key = Metadata.Name.ToString();
		switch (Metadata.Type)
		{
		case TraceServices::FTraceSessionMetadata::EType::String:
			MetadataObj->SetStringField(Key, Metadata.StringValue);
			break;
		case TraceServices::FTraceSessionMetadata::EType::Int64:
			// int64 past 2^53 loses precision as a JSON number; emit as a string
			// so a large value round-trips exactly instead of silently drifting.
			MetadataObj->SetStringField(Key, FString::Printf(TEXT("%lld"), Metadata.Int64Value));
			break;
		case TraceServices::FTraceSessionMetadata::EType::Double:
			MetadataObj->SetNumberField(Key, Metadata.DoubleValue);
			break;
		}
	});
	Data->SetObjectField(TEXT("metadata"), MetadataObj);

	// P0-6c: a caller who reconnects to an existing session must be able to
	// learn what the capture contains without re-opening the trace.
	Data->SetObjectField(TEXT("capture_manifest"),
		ClaireonTraceCaptureManifest::Build(*Session->AnalysisSession));

	const FString Summary = FString::Printf(TEXT("Trace session %s: %.3fs, analysis %s"),
		*Session->SessionId,
		Session->AnalysisSession->GetDurationSeconds(),
		Session->AnalysisSession->IsAnalysisComplete() ? TEXT("complete") : TEXT("in progress"));

	return MakeSuccessResult(Data, Summary);
}
