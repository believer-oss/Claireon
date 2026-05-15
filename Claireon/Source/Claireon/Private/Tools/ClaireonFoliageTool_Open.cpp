// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonFoliageTool_Open.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonLandscapeHelpers.h"
#include "ClaireonSessionManager.h"
#include "Editor.h"
#include "InstancedFoliageActor.h"
#include "Engine/World.h"
#include "Engine/Level.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonFoliageTool_Open::GetName() const
{
	return TEXT("claireon.foliage_open");
}

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
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("Editor not available"));
	}

	UWorld* World = GEditor->GetEditorWorldContext().World();
	if (!World)
	{
		return MakeErrorResult(TEXT("No editor world loaded"));
	}

	EnsureDelegateRegistered();

	FString Error;
	AInstancedFoliageActor* IFA = ClaireonLandscapeHelpers::GetOrCreateFoliageActor(World, Error);
	if (!IFA)
	{
		return MakeErrorResult(Error);
	}

	const FString LevelPath = World->PersistentLevel->GetPathName();
	FMCPOpenSessionResult SessionResult = FClaireonSessionManager::Get().OpenSession(LevelPath, FoliageSessionToolName);
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

	const FString SessionId = SessionResult.SessionId;
	FFoliageEditToolData& Data = ToolData.FindOrAdd(SessionId);
	Data.FoliageActor = IFA;
	Data.LastOperationStatus = TEXT("Session opened");

	return BuildStateResponse(SessionId, &Data);
}
