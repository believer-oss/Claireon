// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TraceOpen.h"
#include "ClaireonLog.h"
#include "ClaireonTraceCaptureManifest.h"
#include "ClaireonTraceSession.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"

FString ClaireonTool_TraceOpen::GetCategory() const { return TEXT("trace"); }
FString ClaireonTool_TraceOpen::GetOperation() const { return TEXT("open"); }

FString ClaireonTool_TraceOpen::GetDescription() const
{
	return TEXT("Load a .utrace file, run analysis, return session ID with basic info (duration, frame count)");
}

TSharedPtr<FJsonObject> ClaireonTool_TraceOpen::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> FilePathProp = MakeShared<FJsonObject>();
	FilePathProp->SetStringField(TEXT("type"), TEXT("string"));
	FilePathProp->SetStringField(TEXT("description"),
		TEXT("Absolute path to a .utrace file (e.g. 'D:/traces/20260219_154240.utrace')"));
	Properties->SetObjectField(TEXT("filePath"), FilePathProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("filePath")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_TraceOpen::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString FilePath;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("filePath"), FilePath) || FilePath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: filePath"));
	}

	if (!FPaths::FileExists(FilePath))
	{
		return MakeErrorResult(FString::Printf(TEXT("Trace file not found: %s"), *FilePath));
	}

	FString Error;
	FClaireonTraceSession* Session = FClaireonTraceSessionManager::Get().OpenSession(FilePath, Error);
	if (!Session)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to open trace: %s"), *Error));
	}

	// Get frame count and duration from the frame provider
	int32 FrameCount = 0;
	double DurationMs = 0.0;
	TSharedPtr<FJsonObject> CaptureManifest;

	if (Session->AnalysisSession.IsValid())
	{
		TraceServices::FAnalysisSessionReadScope ReadScope(*Session->AnalysisSession);
		const TraceServices::IFrameProvider* FrameProvider = Session->GetFrameProvider();
		if (FrameProvider)
		{
			FrameCount = (int32)FrameProvider->GetFrameCount(TraceFrameType_Game);
		}
		if (FrameCount > 0 && FrameProvider)
		{
			const TraceServices::FFrame* FirstFrame = FrameProvider->GetFrame(TraceFrameType_Game, 0);
			const TraceServices::FFrame* LastFrame = FrameProvider->GetFrame(TraceFrameType_Game, FrameCount - 1);
			if (FirstFrame && LastFrame)
			{
				// LastFrame->EndTime is +inf while that frame is still open, which is
				// how this field used to emit the bare token `inf` and make the whole
				// result unparseable. The result-boundary guard now turns it into
				// null; unterminated_frames on trace_get_frame_stats says why.
				DurationMs = (LastFrame->EndTime - FirstFrame->StartTime) * 1000.0;
			}
		}

		// P0-6c: built inside the same read scope, so the manifest costs no extra
		// locking on top of the frame reads above.
		CaptureManifest = ClaireonTraceCaptureManifest::Build(*Session->AnalysisSession);
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), Session->SessionId);
	Data->SetStringField(TEXT("trace_file"), FilePath);
	Data->SetNumberField(TEXT("duration_ms"), DurationMs);
	Data->SetNumberField(TEXT("frame_count"), FrameCount);
	if (CaptureManifest.IsValid())
	{
		Data->SetObjectField(TEXT("capture_manifest"), CaptureManifest);
	}

	const FString Summary = FString::Printf(TEXT("Opened trace: %d frames, %.0fms"),
		FrameCount, DurationMs);

	return MakeSuccessResult(Data, Summary);
}
