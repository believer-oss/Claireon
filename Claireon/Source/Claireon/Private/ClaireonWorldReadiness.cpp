// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "ClaireonWorldReadiness.h"
#include "ClaireonBridge.h"
#include "Editor.h"
#include "Engine/World.h"

FClaireonWorldReadinessResult FClaireonWorldReadiness::Check()
{
	FClaireonWorldReadinessResult Result;

	if (!GEditor)
	{
		Result.Reason = EClaireonWorldNotReadyReason::NoEditor;
		Result.Message = TEXT("Editor is not available.");
		Result.RecoveryHint = TEXT("Wait for the editor to finish initializing.");
		return Result;
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		Result.Reason = EClaireonWorldNotReadyReason::NoWorld;
		Result.Message = TEXT("No world loaded.");
		Result.RecoveryHint = TEXT("Use open_map to load a map first.");
		return Result;
	}

	if (World->bIsTearingDown)
	{
		Result.Reason = EClaireonWorldNotReadyReason::WorldTearingDown;
		Result.Message = TEXT("The world is being torn down.");
		Result.RecoveryHint = TEXT("Wait for the current operation to complete, then retry.");
		return Result;
	}

	if (FClaireonBridge::HasDeferredWorldTransition())
	{
		Result.Reason = EClaireonWorldNotReadyReason::DeferredWorldTransition;
		Result.Message = TEXT("A map transition is pending.");
		Result.RecoveryHint = TEXT("Wait for the map load or PIE transition to complete before using this tool.");
		return Result;
	}

	Result.bReady = true;
	return Result;
}

bool FClaireonWorldReadiness::IsReady()
{
	return Check().bReady;
}
