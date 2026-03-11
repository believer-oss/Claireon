// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_AssetCook.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FString ClaireonTool_AssetCook::GetName() const
{
	return TEXT("editor.asset.cook");
}

FString ClaireonTool_AssetCook::GetDescription() const
{
	return TEXT("[DEPRECATED] Cook content for a target platform. "
				"Use Scripts/Utilities/Invoke-CookContent.ps1 directly instead.");
}

TSharedPtr<FJsonObject> ClaireonTool_AssetCook::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// platform (required)
	TSharedPtr<FJsonObject> PlatformProp = MakeShared<FJsonObject>();
	PlatformProp->SetStringField(TEXT("type"), TEXT("string"));
	PlatformProp->SetStringField(TEXT("description"),
		TEXT("Target platform to cook for (e.g. 'Windows', 'Linux', 'PS5', 'XSX')"));
	Properties->SetObjectField(TEXT("platform"), PlatformProp);

	// maps (optional)
	TSharedPtr<FJsonObject> MapsProp = MakeShared<FJsonObject>();
	MapsProp->SetStringField(TEXT("type"), TEXT("array"));
	TSharedPtr<FJsonObject> MapsItems = MakeShared<FJsonObject>();
	MapsItems->SetStringField(TEXT("type"), TEXT("string"));
	MapsProp->SetObjectField(TEXT("items"), MapsItems);
	MapsProp->SetStringField(TEXT("description"),
		TEXT("Specific maps to cook. If omitted, cooks all maps."));
	Properties->SetObjectField(TEXT("maps"), MapsProp);

	// iterativeCooking (optional)
	TSharedPtr<FJsonObject> IterProp = MakeShared<FJsonObject>();
	IterProp->SetStringField(TEXT("type"), TEXT("boolean"));
	IterProp->SetStringField(TEXT("description"),
		TEXT("Enable iterative cooking to only cook changed content (default: true)"));
	Properties->SetObjectField(TEXT("iterativeCooking"), IterProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	// Required fields
	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("platform")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_AssetCook::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	// STAGE 016: In-process cooking via UCookOnTheFlyServer::StartCookByTheBook() exists but is
	// inappropriate for MCP use: it is asynchronous (driven by the editor's FTickableEditorObject
	// tick loop), can only run if GUnrealEd->CookServer was initialized as CookByTheBookFromTheEditor
	// (gated on bDisableCookInEditor setting), requires ITargetPlatform* lookups, and blocks the
	// running editor's GC and DDC during the session. The subprocess approach used here is the
	// correct pattern — matching Scripts/Utilities/Invoke-CookContent.ps1 which is the canonical
	// entry point. Prefer calling that script directly over this MCP tool.
	UE_LOG(LogClaireon, Warning,
		TEXT("[DEPRECATED] %s is deprecated and will be removed in a future release. "
			 "Use Scripts/Utilities/Invoke-CookContent.ps1 directly instead."),
		*GetName());

	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Arguments are required. 'platform' must be specified."));
	}

	// Parse platform
	FString Platform;
	if (!Arguments->TryGetStringField(TEXT("platform"), Platform) || Platform.IsEmpty())
	{
		return MakeErrorResult(TEXT("'platform' is required and must not be empty."));
	}

	// Parse maps (optional)
	TArray<FString> Maps;
	const TArray<TSharedPtr<FJsonValue>>* MapsArray = nullptr;
	if (Arguments->TryGetArrayField(TEXT("maps"), MapsArray) && MapsArray != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& MapValue : *MapsArray)
		{
			FString MapStr;
			if (MapValue.IsValid() && MapValue->TryGetString(MapStr) && !MapStr.IsEmpty())
			{
				Maps.Add(MapStr);
			}
		}
	}

	// Parse iterativeCooking (default: true)
	bool bIterativeCooking = true;
	if (Arguments->HasField(TEXT("iterativeCooking")))
	{
		Arguments->TryGetBoolField(TEXT("iterativeCooking"), bIterativeCooking);
	}

	UE_LOG(LogClaireon, Display,
		TEXT("[MCP] editor.asset.cook: platform=%s, maps=%d, iterative=%s"),
		*Platform, Maps.Num(), bIterativeCooking ? TEXT("true") : TEXT("false"));

	// Check for the cook script
	const FString ScriptPath = FPaths::Combine(
		FPaths::ProjectDir(), TEXT("Scripts/Utilities/Invoke-CookContent.ps1"));
	const FString AbsScriptPath = FPaths::ConvertRelativePathToFull(ScriptPath);

	FString ExePath;
	FString ExeArgs;

	if (FPaths::FileExists(AbsScriptPath))
	{
		// Delegate to existing script
		ExePath = TEXT("powershell.exe");
		ExeArgs = FString::Printf(
			TEXT("-ExecutionPolicy Bypass -File \"%s\" -Platform %s"),
			*AbsScriptPath, *Platform);

		if (Maps.Num() > 0)
		{
			ExeArgs += FString::Printf(TEXT(" -Maps @(\"%s\")"),
				*FString::Join(Maps, TEXT("\",\"")));
		}

		if (bIterativeCooking)
		{
			ExeArgs += TEXT(" -IterativeCooking");
		}
	}
	else
	{
		// Build cook command directly
		const FString EditorCmdPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::EngineDir(), TEXT("Binaries/Win64/UnrealEditor-Cmd.exe")));

		if (!FPaths::FileExists(EditorCmdPath))
		{
			return MakeErrorResult(FString::Printf(
				TEXT("Neither cook script (%s) nor UnrealEditor-Cmd.exe (%s) found."),
				*AbsScriptPath, *EditorCmdPath));
		}

		const FString ProjectFilePath = FPaths::ConvertRelativePathToFull(
			FPaths::GetProjectFilePath());

		ExePath = EditorCmdPath;
		ExeArgs = FString::Printf(
			TEXT("\"%s\" -run=Cook -TargetPlatform=%s"),
			*ProjectFilePath, *Platform);

		if (Maps.Num() > 0)
		{
			ExeArgs += FString::Printf(TEXT(" -Map=%s"),
				*FString::Join(Maps, TEXT("+")));
		}

		if (bIterativeCooking)
		{
			ExeArgs += TEXT(" -iterate");
		}

		ExeArgs += TEXT(" -unattended -nopause");
	}

	// Ensure log output directory exists
	const FString LogDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MCP/Cook"));
	IFileManager::Get().MakeDirectory(*LogDir, true);

	const FString Timestamp = FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"));
	const FString LogFileName = FString::Printf(TEXT("cook_%s_%s.log"), *Platform, *Timestamp);
	const FString LogFilePath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(LogDir, LogFileName));

	// Create pipes for stdout/stderr capture
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

	UE_LOG(LogClaireon, Display, TEXT("[MCP] Launching cook: %s %s"), *ExePath, *ExeArgs);

	const double StartTime = FPlatformTime::Seconds();

	FProcHandle ProcHandle = FPlatformProcess::CreateProc(
		*ExePath, *ExeArgs,
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
		return MakeErrorResult(TEXT("Failed to launch cook process. Could not create subprocess."));
	}

	// Wait for completion with long timeout (1800 seconds for cooking)
	constexpr double TimeoutSeconds = 1800.0;
	FString Output;
	bool bTimedOut = false;

	while (FPlatformProcess::IsProcRunning(ProcHandle))
	{
		Output += FPlatformProcess::ReadPipe(ReadPipe);
		FPlatformProcess::Sleep(0.5f);

		const double Elapsed = FPlatformTime::Seconds() - StartTime;
		if (Elapsed > TimeoutSeconds)
		{
			bTimedOut = true;
			UE_LOG(LogClaireon, Warning,
				TEXT("[MCP] Cook process exceeded timeout of %.0f seconds, killing process"),
				TimeoutSeconds);
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
		TEXT("[MCP] Cook completed: exitCode=%d, duration=%.1fs, timedOut=%s, logFile=%s"),
		ReturnCode, DurationSeconds,
		bTimedOut ? TEXT("true") : TEXT("false"),
		*LogFilePath);

	// Build result
	FString Result;
	if (bTimedOut)
	{
		Result += FString::Printf(
			TEXT("Cook TIMED OUT after %.1f seconds (limit: %.0f seconds)\n"),
			DurationSeconds, TimeoutSeconds);
	}
	else if (ReturnCode == 0)
	{
		Result += TEXT("Cook completed successfully\n");
	}
	else
	{
		Result += FString::Printf(TEXT("Cook FAILED with exit code %d\n"), ReturnCode);
	}

	Result += FString::Printf(TEXT("Platform: %s\n"), *Platform);
	Result += FString::Printf(TEXT("Iterative: %s\n"), bIterativeCooking ? TEXT("true") : TEXT("false"));
	Result += FString::Printf(TEXT("Exit Code: %d\n"), ReturnCode);
	Result += FString::Printf(TEXT("Duration: %.1f seconds\n"), DurationSeconds);
	Result += FString::Printf(TEXT("Log File: %s\n"), *LogFilePath);

	if (bTimedOut)
	{
		Result += TEXT("Timed Out: true\n");
	}

	if (Maps.Num() > 0)
	{
		Result += FString::Printf(TEXT("Maps: %s\n"), *FString::Join(Maps, TEXT(", ")));
	}

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

	const FString DeprecationNotice = FString::Printf(
		TEXT("[DEPRECATED] %s will be removed. Use Scripts/Utilities/Invoke-CookContent.ps1 directly.\n\n"),
		*GetName());
	const FString FinalText = DeprecationNotice + Result;
	return bIsError ? MakeErrorResult(FinalText) : MakeSuccessResult(nullptr, FinalText);
}
