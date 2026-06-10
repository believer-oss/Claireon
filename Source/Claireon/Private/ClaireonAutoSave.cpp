// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonAutoSave.h"
#include "ClaireonLog.h"
#include "ClaireonSettings.h"
#include "FileHelpers.h"
#include "Editor.h"
#include "HAL/PlatformTime.h"

double FClaireonAutoSave::LastSaveTimeSeconds = 0.0;
bool FClaireonAutoSave::bCrashFlag = false;

int32 FClaireonAutoSave::SaveIfNeeded(bool bIsPythonExecution)
{
	const UClaireonSettings* Settings = UClaireonSettings::Get();
	if (!Settings)
	{
		return 0;
	}

	// Master toggle
	if (!Settings->bEnableAutoSave)
	{
		return 0;
	}

	// Per-integration-point toggle
	if (bIsPythonExecution && !Settings->bAutoSaveBeforePythonExecution)
	{
		return 0;
	}
	if (!bIsPythonExecution && !Settings->bAutoSaveBeforeDeferredActions)
	{
		return 0;
	}

	// Crash flag -- do not save potentially corrupted state
	if (bCrashFlag)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[AutoSave] Skipped -- crash flag is set. Clear with a successful execution."));
		return 0;
	}

	// Skip during PIE to avoid in-flight state corruption
	if (GEditor && GEditor->IsPlaySessionInProgress())
	{
		return 0;
	}

	// Debounce
	const double NowSeconds = FPlatformTime::Seconds();
	if ((NowSeconds - LastSaveTimeSeconds) < Settings->AutoSaveDebounceSeconds)
	{
		return 0;
	}

	// Collect dirty packages
	TArray<UPackage*> DirtyPackages;
	FEditorFileUtils::GetDirtyWorldPackages(DirtyPackages);
	FEditorFileUtils::GetDirtyContentPackages(DirtyPackages);

	if (DirtyPackages.Num() == 0)
	{
		return 0;
	}

	UE_LOG(LogClaireon, Log, TEXT("[AutoSave] Saving %d dirty package(s) before %s..."),
		DirtyPackages.Num(),
		bIsPythonExecution ? TEXT("Python execution") : TEXT("deferred action"));

	const bool bSaved = UEditorLoadingAndSavingUtils::SavePackages(DirtyPackages, false);

	if (bSaved)
	{
		LastSaveTimeSeconds = FPlatformTime::Seconds();
		UE_LOG(LogClaireon, Log, TEXT("[AutoSave] Saved %d package(s) successfully."), DirtyPackages.Num());
		return DirtyPackages.Num();
	}

	UE_LOG(LogClaireon, Warning, TEXT("[AutoSave] SavePackages returned false (some packages may not have saved)."));
	// Still update timestamp to avoid retry-spam
	LastSaveTimeSeconds = FPlatformTime::Seconds();
	return 0;
}

void FClaireonAutoSave::SetCrashFlag()
{
	bCrashFlag = true;
	UE_LOG(LogClaireon, Warning, TEXT("[AutoSave] Crash flag SET -- auto-save suppressed until cleared."));
}

void FClaireonAutoSave::ClearCrashFlag()
{
	if (bCrashFlag)
	{
		bCrashFlag = false;
		UE_LOG(LogClaireon, Log, TEXT("[AutoSave] Crash flag cleared."));
	}
}

bool FClaireonAutoSave::IsCrashFlagSet()
{
	return bCrashFlag;
}
