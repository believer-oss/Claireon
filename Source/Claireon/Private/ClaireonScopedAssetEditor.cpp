// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonScopedAssetEditor.h"

#include "ClaireonLog.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Toolkits/AssetEditorToolkit.h"
#include "UObject/Object.h"

namespace ClaireonAssetEditorWindow
{
	EClaireonEditorAvailability CheckAvailability()
	{
		if (!::IsValid(GEditor))
		{
			return EClaireonEditorAvailability::NoEditorEngine;
		}

		// GEditor can exist without a Slate application.
		if (!FSlateApplication::IsInitialized())
		{
			return EClaireonEditorAvailability::NoSlateApplication;
		}

		if (!::IsValid(GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()))
		{
			return EClaireonEditorAvailability::NoAssetEditorSubsystem;
		}

		return EClaireonEditorAvailability::Available;
	}

	const TCHAR* ToWireString(EClaireonEditorAvailability Availability)
	{
		switch (Availability)
		{
		case EClaireonEditorAvailability::Available:              return TEXT("available");
		case EClaireonEditorAvailability::NoEditorEngine:         return TEXT("no_editor_engine");
		case EClaireonEditorAvailability::NoSlateApplication:     return TEXT("no_slate_application");
		case EClaireonEditorAvailability::NoAssetEditorSubsystem: return TEXT("no_asset_editor_subsystem");
		}
		return TEXT("unknown");
	}

	const TCHAR* ToWireString(EClaireonEditorWindowState State)
	{
		switch (State)
		{
		case EClaireonEditorWindowState::NotOpened:   return TEXT("not_opened");
		case EClaireonEditorWindowState::Opened:      return TEXT("opened");
		case EClaireonEditorWindowState::AlreadyOpen: return TEXT("already_open");
		}
		return TEXT("unknown");
	}

	FString DescribeMissingPrecondition(EClaireonEditorAvailability Availability)
	{
		switch (Availability)
		{
		case EClaireonEditorAvailability::Available:
			return FString();
		case EClaireonEditorAvailability::NoEditorEngine:
			return TEXT("no editor engine (GEditor is null), so there is nothing to host an asset editor");
		case EClaireonEditorAvailability::NoSlateApplication:
			return TEXT("no Slate application in this process (a commandlet or -nullrhi run), so no window can exist");
		case EClaireonEditorAvailability::NoAssetEditorSubsystem:
			return TEXT("the asset-editor subsystem did not resolve, so no asset editor can be opened");
		}
		return TEXT("an unrecognized precondition was missing");
	}

	TSharedPtr<FAssetEditorToolkit> FindToolkitForAsset(UObject* Asset)
	{
		if (!::IsValid(Asset) || CheckAvailability() != EClaireonEditorAvailability::Available)
		{
			return nullptr;
		}

		UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		IAssetEditorInstance* Instance = Subsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false);
		if (!Instance)
		{
			return nullptr;
		}

		// Registered instances are expected to derive from FAssetEditorToolkit, including
		// the UAssetEditor family through FBaseAssetToolkit.
		FAssetEditorToolkit* Toolkit = static_cast<FAssetEditorToolkit*>(Instance);
		return Toolkit->AsShared();
	}

	bool IsInstanceRegistered(UObject* Asset, const FAssetEditorToolkit* Instance)
	{
		if (!::IsValid(Asset) || !Instance || CheckAvailability() != EClaireonEditorAvailability::Available)
		{
			return false;
		}

		// An asset can have several editors; check the recorded instance.
		UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		for (const IAssetEditorInstance* Candidate : Subsystem->FindEditorsForAsset(Asset))
		{
			if (static_cast<const FAssetEditorToolkit*>(Candidate) == Instance)
			{
				return true;
			}
		}
		return false;
	}

	FClaireonEditorOpenOutcome OpenForSession(UObject* Asset)
	{
		FClaireonEditorOpenOutcome Outcome;

		if (!::IsValid(Asset))
		{
			Outcome.Reason = TEXT("no asset was supplied to open an editor for");
			return Outcome;
		}

		Outcome.Availability = CheckAvailability();
		if (Outcome.Availability != EClaireonEditorAvailability::Available)
		{
			Outcome.Reason = DescribeMissingPrecondition(Outcome.Availability);
			return Outcome;
		}

		UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		if (Subsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false))
		{
			Outcome.State = EClaireonEditorWindowState::AlreadyOpen;
			return Outcome;
		}

		Subsystem->OpenEditorForAsset(Asset);
		if (Subsystem->FindEditorForAsset(Asset, /*bFocusIfOpen=*/false))
		{
			Outcome.State = EClaireonEditorWindowState::Opened;
			UE_LOG(LogClaireon, Verbose, TEXT("[AssetEditorWindow] Opened %s"), *Asset->GetPathName());
			return Outcome;
		}

		Outcome.Reason = FString::Printf(
			TEXT("the asset-editor subsystem opened no editor for '%s'"), *Asset->GetPathName());
		UE_LOG(LogClaireon, Warning, TEXT("[AssetEditorWindow] %s"), *Outcome.Reason);
		return Outcome;
	}
}

FClaireonScopedAssetEditor::FClaireonScopedAssetEditor(UObject* InAsset, bool bInCloseOnDestroy)
	: Asset(InAsset)
	, bCloseOnDestroy(bInCloseOnDestroy)
{
	Outcome = ClaireonAssetEditorWindow::OpenForSession(InAsset);
	if (Outcome.WasOpenedOrAlreadyOpen())
	{
		Toolkit = ClaireonAssetEditorWindow::FindToolkitForAsset(InAsset);
	}
}

FClaireonScopedAssetEditor::~FClaireonScopedAssetEditor()
{
	if (!bCloseOnDestroy || WasAlreadyOpen() || !Asset.IsValid())
	{
		return;
	}

	if (ClaireonAssetEditorWindow::CheckAvailability() != EClaireonEditorAvailability::Available)
	{
		return;
	}

	// Close only the recorded instance, and only if it remains registered.
	if (!Toolkit.IsValid())
	{
		UE_LOG(LogClaireon, Verbose,
			TEXT("[AssetEditorWindow] Opened %s but recorded no toolkit; closing nothing"),
			*Asset->GetPathName());
		return;
	}
	if (!ClaireonAssetEditorWindow::IsInstanceRegistered(Asset.Get(), Toolkit.Get()))
	{
		UE_LOG(LogClaireon, Verbose,
			TEXT("[AssetEditorWindow] The editor this scope opened for %s is already closed"),
			*Asset->GetPathName());
		return;
	}

	Toolkit->CloseWindow(EAssetEditorCloseReason::CloseAllEditorsForAsset);
	UE_LOG(LogClaireon, Verbose, TEXT("[AssetEditorWindow] Closed %s"), *Asset->GetPathName());
}
