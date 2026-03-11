// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LiveCodingReload.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"

#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#endif

FString ClaireonTool_LiveCodingReload::GetName() const
{
	return TEXT("live_coding_reload");
}

FString ClaireonTool_LiveCodingReload::GetCategory() const
{
	return TEXT("build");
}

FString ClaireonTool_LiveCodingReload::GetDescription() const
{
	return TEXT("Trigger a Live Coding reload for .cpp changes. Blocks if header files have changed (requires full rebuild).");
}

TSharedPtr<FJsonObject> ClaireonTool_LiveCodingReload::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> ForceProp = MakeShared<FJsonObject>();
	ForceProp->SetStringField(TEXT("type"), TEXT("boolean"));
	ForceProp->SetStringField(TEXT("description"),
		TEXT("Bypass the header-change check and force a Live Coding compile (use with caution)"));
	Properties->SetObjectField(TEXT("force"), ForceProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_LiveCodingReload::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
#if !PLATFORM_WINDOWS
	return MakeErrorResult(TEXT("Live Coding is only available on Windows."));
#else
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (!LiveCoding)
	{
		return MakeErrorResult(TEXT("Live Coding module is not available."));
	}

	if (!LiveCoding->IsEnabledForSession())
	{
		return MakeErrorResult(TEXT("Live Coding is not enabled for this session. Enable it via Editor Preferences > Live Coding, or press Ctrl+Alt+F11."));
	}

	if (LiveCoding->IsCompiling())
	{
		return MakeErrorResult(TEXT("A Live Coding compile is already in progress."));
	}

	// Header change detection (unless force is set)
	bool bForce = false;
	if (Arguments.IsValid())
	{
		Arguments->TryGetBoolField(TEXT("force"), bForce);
	}

	TArray<FString> ChangedHeaders;
	if (!bForce && DetectHeaderChanges(ChangedHeaders))
	{
		FString HeaderList;
		for (const FString& Header : ChangedHeaders)
		{
			if (!HeaderList.IsEmpty())
			{
				HeaderList += TEXT(", ");
			}
			HeaderList += Header;
		}
		return MakeErrorResult(FString::Printf(
			TEXT("Header files have changed — Live Coding only supports .cpp changes. Changed headers: %s. A full editor restart with rebuild is required."),
			*HeaderList));
	}

	const FDateTime StartTime = FDateTime::Now();

	ELiveCodingCompileResult Result;
	bool bStarted = LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &Result);

	const int32 DurationMs = FMath::RoundToInt((FDateTime::Now() - StartTime).GetTotalMilliseconds());

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("duration_ms"), DurationMs);

	TArray<TSharedPtr<FJsonValue>> WarningsArr;
	if (!ChangedHeaders.IsEmpty() && bForce)
	{
		WarningsArr.Add(MakeShared<FJsonValueString>(TEXT("Header change detection was bypassed via force flag")));
	}
	Data->SetArrayField(TEXT("warnings"), WarningsArr);

	FString Summary;
	switch (Result)
	{
	case ELiveCodingCompileResult::Success:
		Data->SetStringField(TEXT("status"), TEXT("success"));
		Summary = FString::Printf(TEXT("Live Coding reload completed successfully (%dms)"), DurationMs);
		return MakeSuccessResult(Data, Summary);

	case ELiveCodingCompileResult::NoChanges:
		Data->SetStringField(TEXT("status"), TEXT("no_changes"));
		Summary = TEXT("No changes detected — nothing to reload");
		return MakeSuccessResult(Data, Summary);

	case ELiveCodingCompileResult::Failure:
		Data->SetStringField(TEXT("status"), TEXT("failed"));
		return MakeErrorResult(FString::Printf(TEXT("Live Coding compile failed (%dms). Check the Output Log for details."), DurationMs));

	case ELiveCodingCompileResult::Cancelled:
		Data->SetStringField(TEXT("status"), TEXT("cancelled"));
		return MakeErrorResult(TEXT("Live Coding compile was cancelled."));

	case ELiveCodingCompileResult::CompileStillActive:
		return MakeErrorResult(TEXT("A Live Coding compile is already in progress."));

	case ELiveCodingCompileResult::NotStarted:
		return MakeErrorResult(TEXT("Live Coding could not start the compile. Ensure Live Coding is enabled and the console is running."));

	case ELiveCodingCompileResult::InProgress:
		Data->SetStringField(TEXT("status"), TEXT("in_progress"));
		Summary = TEXT("Live Coding compile started (not waiting for completion)");
		return MakeSuccessResult(Data, Summary);

	default:
		return MakeErrorResult(FString::Printf(TEXT("Unexpected Live Coding result: %d"), static_cast<int32>(Result)));
	}
#endif
}

bool ClaireonTool_LiveCodingReload::DetectHeaderChanges(TArray<FString>& OutChangedHeaders) const
{
	const FString ProjectDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	// Run git diff --name-only to get modified files in the working tree
	auto RunGitDiff = [&ProjectDir](const FString& ExtraArgs, TArray<FString>& OutLines) -> bool
	{
		FString GitExe = TEXT("git");
		FString Args = FString::Printf(TEXT("diff --name-only %s"), *ExtraArgs);

		void* ReadPipe = nullptr;
		void* WritePipe = nullptr;
		FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

		FProcHandle Proc = FPlatformProcess::CreateProc(
			*GitExe, *Args, false, true, true, nullptr, 0, *ProjectDir, WritePipe, ReadPipe);

		if (!Proc.IsValid())
		{
			FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
			return false;
		}

		FString Output;
		while (FPlatformProcess::IsProcRunning(Proc))
		{
			Output += FPlatformProcess::ReadPipe(ReadPipe);
			FPlatformProcess::Sleep(0.01f);
		}
		Output += FPlatformProcess::ReadPipe(ReadPipe);

		int32 ReturnCode = -1;
		FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
		FPlatformProcess::CloseProc(Proc);
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

		if (ReturnCode != 0)
		{
			return false;
		}

		Output.ParseIntoArrayLines(OutLines);
		return true;
	};

	// Get unstaged changes
	TArray<FString> UnstagedFiles;
	bool bGotUnstaged = RunGitDiff(TEXT(""), UnstagedFiles);

	// Get staged changes
	TArray<FString> StagedFiles;
	bool bGotStaged = RunGitDiff(TEXT("--cached"), StagedFiles);

	if (!bGotUnstaged && !bGotStaged)
	{
		UE_LOG(LogClaireon, Warning, TEXT("Could not run git diff — header change detection skipped"));
		return false;
	}

	// Combine and filter for header files
	TSet<FString> AllFiles;
	for (const FString& F : UnstagedFiles) { AllFiles.Add(F); }
	for (const FString& F : StagedFiles) { AllFiles.Add(F); }

	for (const FString& FilePath : AllFiles)
	{
		if (FilePath.EndsWith(TEXT(".h")) || FilePath.EndsWith(TEXT(".hpp")) || FilePath.EndsWith(TEXT(".inl")))
		{
			OutChangedHeaders.Add(FilePath);
		}
	}

	return OutChangedHeaders.Num() > 0;
}
