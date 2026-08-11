// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonFoliageTool_Open.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonLandscapeHelpers.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"
#include "Editor.h"
#include "InstancedFoliageActor.h"
#include "Engine/World.h"
#include "Engine/Level.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonFoliageTool_Open::GetOperation() const { return TEXT("open"); }

FString ClaireonFoliageTool_Open::GetDescription() const
{
	return TEXT("Open a session on the current editor level's foliage actor. Returns a session_id for subsequent foliage operations.");
}

TSharedPtr<FJsonObject> ClaireonFoliageTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	return Builder.Build();
}

FToolResult ClaireonFoliageTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!IsValid(GEditor))
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!IsValid(World))
	{
		return MakeErrorResult(TEXT("No editor world loaded"));
	}

	EnsureDelegateRegistered();

	FString Error;
	AInstancedFoliageActor* IFA = ClaireonLandscapeHelpers::GetOrCreateFoliageActor(World, Error);
	if (!IsValid(IFA))
	{
		return MakeErrorResult(Error);
	}

	const FString LevelPath = World->PersistentLevel->GetPathName();
	// bAllowUnsavedWorldPackage=true: LevelPath's package IS the current editor world (not a
	// /Game/ asset), so C5 hardening lets FClaireonSessionManager::CanonicalizePath accept an
	// unsaved level's /Temp/Untitled_N package here. Before this, the InvalidAssetPath branch
	// just below was the only thing standing between "File > New Level, then call this tool"
	// and a bogus success (see the comment there) -- now the lock is actually acquired instead
	// of merely failing loudly.
	FMCPOpenSessionResult SessionResult = FClaireonSessionManager::Get().OpenSession(
		LevelPath, FoliageSessionToolName, /*TimeoutMinutes=*/60.0, /*bAllowUnsavedWorldPackage=*/true);
	if (SessionResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		FString BlockInfo = TEXT("another tool");
		if (SessionResult.BlockingSession.IsSet())
		{
			BlockInfo = FString::Printf(TEXT("%s (session %s)"),
				*SessionResult.BlockingSession->ToolName, *SessionResult.BlockingSession->SessionId);
		}
		return MakeErrorResult(FString::Printf(TEXT("Foliage locked by %s"), *BlockInfo));
	}
	// Defect guard: this used to handle only BlockedByOtherTool. On
	// InvalidAssetPath (OpenSession's CanonicalizePath rejected the path) SessionId
	// is empty, and falling through returned a SUCCESS state response carrying an
	// empty session_id -- an unusable handle with no error. This is reachable in
	// normal use: CanonicalizePath used to reject anything not under /Game/, and an
	// unsaved map's persistent level lives at /Temp/Untitled_N -- now allowed above via
	// bAllowUnsavedWorldPackage. Every non-success result must still produce an error
	// here (e.g. a genuinely malformed path from some other future caller shape).
	if (SessionResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *LevelPath));
	}
	if (SessionResult.Result != EOpenSessionResult::Success && SessionResult.Result != EOpenSessionResult::ReusedExistingSession)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("Failed to open a session for %s (unexpected OpenSession result %d)"),
			*LevelPath, static_cast<int32>(SessionResult.Result)));
	}

	const FString SessionId = SessionResult.SessionId;
	FFoliageEditToolData& Data = ToolData.FindOrAdd(SessionId);
	Data.FoliageActor = IFA;
	Data.LastOperationStatus = TEXT("Session opened");

	// World-scoped tool: mode-focus (foliage edit mode) deferred per Q-AUTO-OPEN-WORLD-MODE;
	// EM_Foliage activation requires LevelEditor module not available in Claireon plugin scope.
	UE_LOG(LogClaireon, Verbose, TEXT("[FoliageTool_Open] Session opened; foliage edit-mode focus deferred (world-scoped)"));

	return BuildStateResponse(SessionId, &Data);
}
