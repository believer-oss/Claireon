// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_LiveCodingReload.h"
#include "ClaireonBridge.h"
#include "ClaireonLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"

#if PLATFORM_WINDOWS
#include "Modules/ModuleManager.h"
#include "ILiveCodingModule.h"
#endif

FString ClaireonTool_LiveCodingReload::GetCategory() const { return TEXT("editor"); }
FString ClaireonTool_LiveCodingReload::GetOperation() const { return TEXT("live_coding_reload_async"); }

FString ClaireonTool_LiveCodingReload::GetDescription() const
{
	return TEXT("Trigger a Live Coding reload for .cpp changes. The reload is deferred until "
		"after the current script finishes. Blocks if header files have changed (requires full rebuild).");
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

	// Enqueue — does NOT execute yet
	FClaireonBridge::EnqueueDeferredAction({
		EClaireonDeferredActionType::LiveCodingReload,
		bForce ? TEXT("force") : TEXT("")
	});

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("status"), TEXT("deferred"));
	Data->SetStringField(TEXT("action"), TEXT("live_coding_reload"));

	TArray<TSharedPtr<FJsonValue>> WarningsArr;
	if (!ChangedHeaders.IsEmpty() && bForce)
	{
		WarningsArr.Add(MakeShared<FJsonValueString>(TEXT("Header change detection was bypassed via force flag")));
	}
	Data->SetArrayField(TEXT("warnings"), WarningsArr);

	FString Summary = TEXT("Live Coding reload queued — executes after script completes");
	return MakeSuccessResult(Data, Summary);
#endif
}

void ClaireonTool_LiveCodingReload::ExecuteDeferredLiveCodingReload(const FString& Payload)
{
#if PLATFORM_WINDOWS
	ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
	if (!LiveCoding || !LiveCoding->IsEnabledForSession() || LiveCoding->IsCompiling())
	{
		UE_LOG(LogClaireon, Warning, TEXT("ExecuteDeferredLiveCodingReload: Live Coding not available or already compiling"));
		return;
	}

	ELiveCodingCompileResult Result;
	LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &Result);

	UE_LOG(LogClaireon, Log, TEXT("ExecuteDeferredLiveCodingReload: result=%d"), static_cast<int32>(Result));
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
