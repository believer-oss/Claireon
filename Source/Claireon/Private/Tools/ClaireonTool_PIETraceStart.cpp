// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIETraceStart.h"
#include "ClaireonTraceNamedEvents.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"

FString ClaireonTool_PIETraceStart::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIETraceStart::GetOperation() const { return TEXT("trace_start"); }

FString ClaireonTool_PIETraceStart::GetDescription() const
{
    return TEXT("Start Unreal Insights .utrace recording. Returns the trace file path. Stateless / non-session: starts an editor-wide trace without opening any per-asset session.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIETraceStart::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Output trace filename without extension. Default: trace_<timestamp>"));
		Properties->SetObjectField(TEXT("filename"), Prop);
	}

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Output directory. Default: Saved/Profiling/"));
		Properties->SetObjectField(TEXT("directory"), Prop);
	}

	{
		TSharedPtr<FJsonObject> Prop = MakeShared<FJsonObject>();
		Prop->SetStringField(TEXT("type"), TEXT("string"));
		Prop->SetStringField(TEXT("description"), TEXT("Comma-separated trace channels (default: cpu,frame,bookmark)"));
		Prop->SetStringField(TEXT("default"), TEXT("cpu,frame,bookmark"));
		Properties->SetObjectField(TEXT("channels"), Prop);
	}

	Schema->SetObjectField(TEXT("properties"), Properties);
	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIETraceStart::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!IsValid(GEditor) || !GEditor->IsPlaySessionInProgress())
	{
		return MakeErrorResult(TEXT("PIE is not running"));
	}

	if (FTraceAuxiliary::IsConnected())
	{
		const FString Dest = FTraceAuxiliary::GetTraceDestinationString();
		return MakeErrorResult(FString::Printf(TEXT("A trace is already recording to: %s. Stop it first with pie_trace_stop"), *Dest));
	}

	// Parse arguments
	FString Directory;
	if (!Arguments->TryGetStringField(TEXT("directory"), Directory) || Directory.IsEmpty())
	{
		Directory = FPaths::ProjectSavedDir() / TEXT("Profiling");
	}

	FString Filename;
	if (!Arguments->TryGetStringField(TEXT("filename"), Filename) || Filename.IsEmpty())
	{
		Filename = FString::Printf(TEXT("trace_%s"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	}

	FString Channels;
	if (!Arguments->TryGetStringField(TEXT("channels"), Channels) || Channels.IsEmpty())
	{
		Channels = TEXT("cpu,frame,bookmark");
	}

	// Absolutize before anything touches the filesystem. FTraceAuxiliary::Start does not
	// resolve a relative path the way IPlatformFile does -- it runs the path through
	// ConvertToAbsolutePathForExternalAppForWrite -- so a relative default like
	// ProjectSavedDir()/Profiling ("../../../../MyProject/Saved/Profiling") had the directory
	// created inside the project while the capture itself was written to a different,
	// out-of-repo location. The traceFilePath reported below was the relative string, which
	// no other process could resolve back to the file that was actually produced.
	const FString FullPath = FPaths::ConvertRelativePathToFull(Directory / (Filename + TEXT(".utrace")));

	// Ensure directory exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*FPaths::GetPath(FullPath));

	// P0-2: force named events on BEFORE the capture starts.
	//
	// The engine gates every SCOPE_CYCLE_COUNTER-derived trace event on
	// GCycleStatsShouldEmitNamedEvents (Stats.h:263), while
	// TRACE_CPUPROFILER_EVENT_SCOPE is ungated. Without this, a capture is
	// missing SceneQueryTotal, Physics Tick, World Tick Time and every other
	// stat-derived scope -- while still returning hundreds of scopes and looking
	// complete. The absence of SceneQueryTotal is then indistinguishable from
	// scene queries being cheap, which is how a capture inverts a conclusion.
	//
	// Mirrors FSPerfTraceDefaults / FSPerfTraceDebugSubsystem: save, force on,
	// restore on stop, roll back on a failed start.
	ClaireonTraceNamedEvents::Push();

	const bool bStarted = FTraceAuxiliary::Start(
		FTraceAuxiliary::EConnectionType::File,
		*FullPath,
		*Channels,
		nullptr);

	if (!bStarted)
	{
		// Roll back: a failed start must not leave the flag forced on for the
		// rest of the editor session, silently changing the cost profile of
		// every later measurement.
		ClaireonTraceNamedEvents::Pop();
		return MakeErrorResult(TEXT("Failed to start trace recording. Check channels and output path."));
	}

	const bool bStatNamedEvents = ClaireonTraceNamedEvents::IsEnabled();

	// Data, not just prose: this tool used to return MakeSuccessResult(nullptr, ...)
	// so traceFilePath was reachable only by parsing the summary.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("recording"));
	Data->SetStringField(TEXT("trace_file_path"), FullPath);
	Data->SetStringField(TEXT("channels"), Channels);
	Data->SetBoolField(TEXT("stat_named_events"), bStatNamedEvents);

	const FString Summary = FString::Printf(
		TEXT("Trace recording started (channels: %s, named events: %s). Stop with pie_trace_stop."),
		*Channels,
		bStatNamedEvents ? TEXT("on") : TEXT("off"));

	FToolResult Result = MakeSuccessResult(Data, Summary);

	// The default channel set has no gpu, so a caller who later asks for GPU
	// scopes gets an empty answer that reads as "GPU work is free".
	if (!Channels.Contains(TEXT("gpu"), ESearchCase::IgnoreCase))
	{
		Result.Warnings.Add(FString::Printf(
			TEXT("Capturing without the 'gpu' channel (channels: %s), so this trace will contain no GPU "
			     "timeline events and trace_get_top_scopes(includeGpu=true) will return the same rows as "
			     "includeGpu=false."),
			*Channels));
	}

	return Result;
}
