// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_CommandletRun.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FString ClaireonTool_CommandletRun::GetCategory() const { return TEXT("commandlet"); }
FString ClaireonTool_CommandletRun::GetOperation() const { return TEXT("run"); }

FString ClaireonTool_CommandletRun::GetDescription() const
{
    return TEXT("Execute an Unreal commandlet as a subprocess with timeout. Stateless / non-session: spawns an external editor process and waits for it.");
}

TSharedPtr<FJsonObject> ClaireonTool_CommandletRun::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// commandletName (required)
	TSharedPtr<FJsonObject> NameProp = MakeShared<FJsonObject>();
	NameProp->SetStringField(TEXT("type"), TEXT("string"));
	NameProp->SetStringField(TEXT("description"),
		TEXT("Name of the commandlet to run (e.g. 'ResavePackages', 'FixupRedirects')"));
	Properties->SetObjectField(TEXT("commandletName"), NameProp);

	// commandletArgs (optional)
	TSharedPtr<FJsonObject> ArgsProp = MakeShared<FJsonObject>();
	ArgsProp->SetStringField(TEXT("type"), TEXT("array"));
	TSharedPtr<FJsonObject> ArgsItems = MakeShared<FJsonObject>();
	ArgsItems->SetStringField(TEXT("type"), TEXT("string"));
	ArgsProp->SetObjectField(TEXT("items"), ArgsItems);
	ArgsProp->SetStringField(TEXT("description"),
		TEXT("Additional arguments to pass to the commandlet"));
	Properties->SetObjectField(TEXT("commandletArgs"), ArgsProp);

	// useFullEditor (optional)
	TSharedPtr<FJsonObject> FullEdProp = MakeShared<FJsonObject>();
	FullEdProp->SetStringField(TEXT("type"), TEXT("boolean"));
	FullEdProp->SetStringField(TEXT("description"),
		TEXT("Use UnrealEditor.exe instead of UnrealEditor-Cmd.exe. Default: false"));
	Properties->SetObjectField(TEXT("useFullEditor"), FullEdProp);

	// timeoutSeconds (optional)
	TSharedPtr<FJsonObject> TimeoutProp = MakeShared<FJsonObject>();
	TimeoutProp->SetStringField(TEXT("type"), TEXT("integer"));
	TimeoutProp->SetStringField(TEXT("description"),
		TEXT("Maximum time in seconds before killing the commandlet. Default: 300"));
	Properties->SetObjectField(TEXT("timeoutSeconds"), TimeoutProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("commandletName")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_CommandletRun::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Warning, TEXT("[DEPRECATED] %s is deprecated and will be removed in a future release. Use the external script instead."), *GetName());

	// Parse arguments
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments are required. 'commandletName' must be specified."));
	}

	FString CommandletName;
	if (!Arguments->TryGetStringField(TEXT("commandletName"), CommandletName)
		|| CommandletName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'commandletName' is required and must not be empty."));
	}

	TArray<FString> CommandletArgs;
	const TArray<TSharedPtr<FJsonValue>>* ArgsArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("commandletArgs"), ArgsArray) && ArgsArray != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& ArgValue : *ArgsArray)
		{
			FString ArgStr;
			if (ArgValue.IsValid() && ArgValue->TryGetString(ArgStr))
			{
				CommandletArgs.Add(ArgStr);
			}
		}
	}

	bool bUseFullEditor = false;
	Arguments->TryGetBoolField(TEXT("useFullEditor"), bUseFullEditor);

	int32 TimeoutSeconds = 300;
	{
		double TimeoutDouble = 0.0;
		if (Arguments->TryGetNumberField(TEXT("timeoutSeconds"), TimeoutDouble))
		{
			TimeoutSeconds = FMath::Clamp(static_cast<int32>(TimeoutDouble), 10, 3600);
		}
	}

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] editor.commandlet.run (commandlet=%s, args=%d, fullEditor=%s, timeout=%ds)"),
		*CommandletName, CommandletArgs.Num(),
		bUseFullEditor ? TEXT("true") : TEXT("false"),
		TimeoutSeconds);

	// Find the editor executable
	const FString EditorExeName = bUseFullEditor
		? TEXT("UnrealEditor.exe")
		: TEXT("UnrealEditor-Cmd.exe");

	const FString EditorExePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::EngineDir(), TEXT("Binaries/Win64"), EditorExeName));

	if (!FPaths::FileExists(EditorExePath))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Editor executable not found: %s"), *EditorExePath));
	}

	// Get the project file path
	const FString ProjectFilePath = FPaths::ConvertRelativePathToFull(
		FPaths::GetProjectFilePath());

	if (!FPaths::FileExists(ProjectFilePath))
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Project file not found: %s"), *ProjectFilePath));
	}

	// Build commandlet command line
	// Format: <ProjectPath> -run=<CommandletName> <Args...> -unattended -nopause
	FString CmdArgs = FString::Printf(
		TEXT("\"%s\" -run=%s"), *ProjectFilePath, *CommandletName);

	for (const FString& Arg : CommandletArgs)
	{
		// If the arg contains spaces and isn't already quoted, quote it
		if (Arg.Contains(TEXT(" ")) && !Arg.StartsWith(TEXT("\"")))
		{
			CmdArgs += FString::Printf(TEXT(" \"%s\""), *Arg);
		}
		else
		{
			CmdArgs += FString::Printf(TEXT(" %s"), *Arg);
		}
	}

	CmdArgs += TEXT(" -unattended -nopause");

	// Ensure log output directory exists
	const FString LogDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCP/Commandlets"));
	IFileManager::Get().MakeDirectory(*LogDir, true);

	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString LogFileName = FString::Printf(TEXT("%s_%s.log"), *CommandletName, *Timestamp);
	const FString LogFilePath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(LogDir, LogFileName));

	// Create pipes for stdout/stderr capture
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] Launching commandlet: %s %s"), *EditorExePath, *CmdArgs);

	const double StartTime = FPlatformTime::Seconds();

	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		*EditorExePath, *CmdArgs,
		/* bLaunchDetached */ true,
		/* bLaunchHidden */ false,
		/* bLaunchReallyHidden */ false,
		/* OutProcessID */ nullptr,
		/* PriorityModifier */ 0,
		/* OptionalWorkingDirectory */ nullptr,
		WritePipe, ReadPipe);

	if (!ProcHandle.IsValid())
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to launch commandlet process: %s"), *EditorExePath));
	}

	// Wait for completion with timeout
	const double TimeoutLimit = static_cast<double>(TimeoutSeconds);
	FString Output;
	bool bTimedOut = false;

	while (FPlatformProcess::IsProcRunning(ProcHandle))
	{
		Output += FPlatformProcess::ReadPipe(ReadPipe);
		FPlatformProcess::Sleep(0.5f);

		const double Elapsed = FPlatformTime::Seconds() - StartTime;
		if (Elapsed > TimeoutLimit)
		{
			bTimedOut = true;
			UE_LOG(LogClaireon, Warning,
				TEXT("[MCP] Commandlet '%s' exceeded timeout of %d seconds, killing process"),
				*CommandletName, TimeoutSeconds);
			FPlatformProcess::TerminateProc(ProcHandle, /* bKillTree */ true);
			break;
		}
	}

	// Final read to capture any remaining output
	Output += FPlatformProcess::ReadPipe(ReadPipe);

	int32 ReturnCode = -1;
	FPlatformProcess::GetProcReturnCode(ProcHandle, &ReturnCode);

	const double DurationSeconds = FPlatformTime::Seconds() - StartTime;

	// Cleanup
	FPlatformProcess::CloseProc(ProcHandle);
	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

	// Save output to log file
	FFileHelper::SaveStringToFile(Output, *LogFilePath);

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] Commandlet '%s' completed: exitCode=%d, duration=%.1fs, timedOut=%s, logFile=%s"),
		*CommandletName, ReturnCode, DurationSeconds,
		bTimedOut ? TEXT("true") : TEXT("false"),
		*LogFilePath);

	// Build result
	FString Result;
	if (bTimedOut)
	{
		Result += FString::Printf(
			TEXT("Commandlet '%s' TIMED OUT after %.1f seconds (limit: %d seconds)\n"),
			*CommandletName, DurationSeconds, TimeoutSeconds);
	}
	else if (ReturnCode == 0)
	{
		Result += FString::Printf(
			TEXT("Commandlet '%s' completed successfully\n"), *CommandletName);
	}
	else
	{
		Result += FString::Printf(
			TEXT("Commandlet '%s' FAILED with exit code %d\n"), *CommandletName, ReturnCode);
	}

	Result += FString::Printf(TEXT("Commandlet: %s\n"), *CommandletName);
	Result += FString::Printf(TEXT("Exit Code: %d\n"), ReturnCode);
	Result += FString::Printf(TEXT("Duration: %.1f seconds\n"), DurationSeconds);
	Result += FString::Printf(TEXT("Timed Out: %s\n"), bTimedOut ? TEXT("true") : TEXT("false"));
	Result += FString::Printf(TEXT("Log File: %s\n"), *LogFilePath);
	Result += FString::Printf(TEXT("Editor Exe: %s\n"), *EditorExeName);

	// Include last portion of output for immediate feedback
	const int32 MaxOutputChars = 2000;
	if (Output.Len() > MaxOutputChars)
	{
		Result += TEXT("\n--- Last output ---\n");
		Result += Output.Right(MaxOutputChars);
	}
	else if (!Output.IsEmpty())
	{
		Result += TEXT("\n--- Output ---\n");
		Result += Output;
	}

	const bool bIsError = (ReturnCode != 0) || bTimedOut;

	FString DeprecationNotice = FString::Printf(
		TEXT("[DEPRECATED] %s will be removed. Use Scripts/Utilities/Invoke-Commandlet.ps1 instead.\n\n"),
		*GetName());
	const FString FinalText = DeprecationNotice + Result;
	return bIsError ? MakeErrorResult(FinalText) : MakeSuccessResult(nullptr, FinalText);
}
