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

int32 FClaireonAutoSave::SaveIfNeeded(bool bIsPythonExecution, TArray<FString>* OutSavedPackageNames)
{
	if (OutSavedPackageNames)
	{
		OutSavedPackageNames->Reset();
	}

	const UClaireonSettings* Settings = UClaireonSettings::Get();
	if (!IsValid(Settings))
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
	if (IsValid(GEditor) && GEditor->IsPlaySessionInProgress())
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

		// P0-7: report WHICH packages were written, not just how many.
		//
		// Both call sites used to discard the count entirely and the only trace
		// was the log line above, so a temp edit persisted into a tracked LFS
		// umap with nothing in the result envelope to show for it. Names are
		// captured here, at the one place that knows them.
		if (OutSavedPackageNames)
		{
			OutSavedPackageNames->Reserve(DirtyPackages.Num());
			for (const UPackage* Package : DirtyPackages)
			{
				if (Package)
				{
					OutSavedPackageNames->Add(Package->GetName());
				}
			}
		}

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
