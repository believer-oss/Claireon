// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIETraceStop.h"
#include "ClaireonTraceNamedEvents.h"
#include "ProfilingDebugging/TraceAuxiliary.h"

FString ClaireonTool_PIETraceStop::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIETraceStop::GetOperation() const { return TEXT("trace_stop"); }

FString ClaireonTool_PIETraceStop::GetDescription() const
{
	return TEXT("Stop Unreal Insights trace recording and return the trace file path. Use trace_open to analyze.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIETraceStop::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));
	// Parameterless, but the empty properties object is not decoration: the
	// argument gate stays permissive for a schema with no "properties" at all,
	// so omitting it opts this tool out of undeclared-argument rejection.
	Schema->SetObjectField(TEXT("properties"), MakeShared<FJsonObject>());
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIETraceStop::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!FTraceAuxiliary::IsConnected())
	{
		return MakeSuccessResult(nullptr, TEXT("status: not_recording\nmessage: No trace is currently recording."));
	}

	const FString Destination = FTraceAuxiliary::GetTraceDestinationString();
	const bool bStatNamedEvents = ClaireonTraceNamedEvents::IsEnabled();

	FTraceAuxiliary::Stop();

	// P0-2: restore whatever GCycleStatsShouldEmitNamedEvents was before the
	// capture forced it on. Reported above rather than after, so the payload
	// describes the capture that just ended rather than the state after cleanup.
	ClaireonTraceNamedEvents::Pop();

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("stopped"));
	Data->SetStringField(TEXT("trace_file_path"), Destination);
	Data->SetBoolField(TEXT("stat_named_events"), bStatNamedEvents);

	const FString Summary = FString::Printf(
		TEXT("Trace recording stopped (named events: %s). Analyze with trace_open."),
		bStatNamedEvents ? TEXT("on") : TEXT("off"));

	return MakeSuccessResult(Data, Summary);
}
