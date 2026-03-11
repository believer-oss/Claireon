// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIETraceStop.h"
#include "ProfilingDebugging/TraceAuxiliary.h"

FString ClaireonTool_PIETraceStop::GetName() const
{
	return TEXT("editor.pie.trace.stop");
}

FString ClaireonTool_PIETraceStop::GetDescription() const
{
	return TEXT("Stop Unreal Insights trace recording and return the trace file path. Use editor.trace.open to analyze.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIETraceStop::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIETraceStop::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!FTraceAuxiliary::IsConnected())
	{
		return MakeSuccessResult(nullptr, TEXT("status: not_recording\nmessage: No trace is currently recording."));
	}

	const FString Destination = FTraceAuxiliary::GetTraceDestinationString();
	FTraceAuxiliary::Stop();

	FString Output = FString::Printf(
		TEXT("status: stopped\n")
		TEXT("traceFilePath: %s\n")
		TEXT("message: Trace recording stopped. Use editor.trace.open to analyze the trace."),
		*Destination);

	return MakeSuccessResult(nullptr, Output);
}
