// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceTool_Open.h"
#include "Tools/ClaireonSequenceHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonLevelSequenceEditInternal.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "LevelSequence.h"
#include "UObject/Package.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonLevelSequenceTool_Open::GetName() const
{
	return TEXT("claireon.level_sequence_open");
}

TArray<FString> ClaireonLevelSequenceTool_Open::GetSearchKeywords() const
{
	return {TEXT("sequence"), TEXT("sequencer"), TEXT("cinematic"), TEXT("keyframe"), TEXT("track"), TEXT("open"), TEXT("session")};
}

FString ClaireonLevelSequenceTool_Open::GetDescription() const
{
	return TEXT("Open a Level Sequence asset for editing. Returns a session_id for subsequent operations. "
				"Pass create_if_missing=true to create a new sequence at the path if none exists.");
}

TSharedPtr<FJsonObject> ClaireonLevelSequenceTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the Level Sequence asset to open (e.g. /Game/Cinematics/LS_Foo)."), true);
	Builder.AddBoolean(TEXT("create_if_missing"), TEXT("If true and the sequence does not exist, create a new Level Sequence at asset_path."));
	return Builder.Build();
}

FToolResult ClaireonLevelSequenceTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!Arguments.IsValid())
	{
		return MakeErrorResult(TEXT("Missing arguments"));
	}

	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	bool bCreateIfMissing = false;
	Arguments->TryGetBoolField(TEXT("create_if_missing"), bCreateIfMissing);

	FString LoadError;
	ULevelSequence* Sequence = FClaireonSequenceHelpers::LoadLevelSequenceAsset(AssetPath, LoadError);
	if (!Sequence && bCreateIfMissing)
	{
		ClaireonPathResolver::FResolveResult Resolved = ClaireonPathResolver::Resolve(AssetPath);
		FString TargetPath = Resolved.bSuccess ? Resolved.ResolvedPath.Path : AssetPath;
		FString PackageName = TargetPath;
		int32 DotIdx;
		if (PackageName.FindChar(TEXT('.'), DotIdx))
		{
			PackageName = PackageName.Left(DotIdx);
		}
		FString CreateError;
		Sequence = ClaireonLevelSequenceInternal::CreateLevelSequenceAtPath(PackageName, CreateError);
		if (!Sequence)
		{
			return MakeErrorResult(FString::Printf(TEXT("create_if_missing failed: %s"), *CreateError));
		}
	}
	if (!Sequence)
	{
		return MakeErrorResult(LoadError.IsEmpty() ? TEXT("Failed to load Level Sequence") : LoadError);
	}

	EnsureDelegateRegistered();

	const FString ResolvedAssetPath = Sequence->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(
		ResolvedAssetPath, LevelSequenceSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		const FTimespan Elapsed = FDateTime::UtcNow() - Blocker.LastAccessTime;
		return MakeErrorResult(FString::Printf(
			TEXT("Asset is locked by %s session %s (last activity %dm %ds ago). Close that session first."),
			*Blocker.ToolName, *Blocker.SessionId,
			static_cast<int32>(Elapsed.GetTotalMinutes()),
			static_cast<int32>(Elapsed.GetTotalSeconds()) % 60));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *ResolvedAssetPath));
	}
	const FString SessionId = OpenResult.SessionId;

	FSequenceEditToolData NewData;
	NewData.Sequence = Sequence;
	NewData.LastOperationStatus = bCreateIfMissing && !Sequence->GetOutermost()->IsFullyLoaded()
		? TEXT("Session opened (new sequence)")
		: TEXT("Session opened");
	NewData.bSuppressOutput = false;
	ToolData.Add(SessionId, MoveTemp(NewData));

	return BuildStateResponse(SessionId, ToolData.Find(SessionId));
}
