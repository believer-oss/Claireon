// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_PIETraceStart.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"

FString ClaireonTool_PIETraceStart::GetName() const
{
	return TEXT("editor.pie.trace.start");
}

FString ClaireonTool_PIETraceStart::GetDescription() const
{
	return TEXT("Start Unreal Insights .utrace recording. Returns the trace file path.");
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
	if (!GEditor || !GEditor->IsPlaySessionInProgress())
	{
		return MakeErrorResult(TEXT("PIE is not running"));
	}

	if (FTraceAuxiliary::IsConnected())
	{
		const FString Dest = FTraceAuxiliary::GetTraceDestinationString();
		return MakeErrorResult(FString::Printf(TEXT("A trace is already recording to: %s. Stop it first with editor.pie.trace.stop"), *Dest));
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

	// Ensure directory exists
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	PlatformFile.CreateDirectoryTree(*Directory);

	const FString FullPath = Directory / (Filename + TEXT(".utrace"));

	const bool bStarted = FTraceAuxiliary::Start(
		FTraceAuxiliary::EConnectionType::File,
		*FullPath,
		*Channels,
		nullptr);

	if (!bStarted)
	{
		return MakeErrorResult(TEXT("Failed to start trace recording. Check channels and output path."));
	}

	FString Output = FString::Printf(
		TEXT("status: recording\n")
		TEXT("traceFilePath: %s\n")
		TEXT("channels: %s\n")
		TEXT("message: Trace recording started. Use editor.pie.trace.stop to stop and get results."),
		*FullPath,
		*Channels);

	return MakeSuccessResult(nullptr, Output);
}
