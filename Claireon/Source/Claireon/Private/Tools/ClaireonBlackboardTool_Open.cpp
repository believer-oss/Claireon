// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlackboardTool_Open.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "BehaviorTree/BlackboardData.h"
#include "Misc/Paths.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBlackboardTool_Open::GetOperation() const { return TEXT("open"); }

TArray<FString> ClaireonBlackboardTool_Open::GetSearchKeywords() const
{
	return {TEXT("bb"), TEXT("blackboard"), TEXT("behavior"), TEXT("tree"), TEXT("keys"), TEXT("open"), TEXT("session")};
}

FString ClaireonBlackboardTool_Open::GetDescription() const
{
	return TEXT("Open a Blackboard Data asset for transactional editing and lock it under tool name "
				"'blackboard_edit' so only one cohort holds it at a time. Returns a session_id used by "
				"all subsequent blackboard session ops (add_key, rename_key, set_key_type, save, close). "
				"Also surfaces the asset editor when running headed.");
}

TSharedPtr<FJsonObject> ClaireonBlackboardTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the Blackboard Data asset to edit."), true);
	return Builder.Build();
}

FToolResult ClaireonBlackboardTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	FString Error;
	UBlackboardData* BB = ClaireonBehaviorTreeHelpers::LoadBlackboardAsset(AssetPath, Error);
	if (!BB)
	{
		return MakeErrorResult(Error);
	}

	EnsureDelegateRegistered();

	const FString ResolvedAssetPath = BB->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, BlackboardSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by %s session %s"), *Blocker.ToolName, *Blocker.SessionId));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *ResolvedAssetPath));
	}
	const FString SessionId = OpenResult.SessionId;

	FBlackboardEditToolData NewData;
	NewData.BlackboardData = BB;
	NewData.LastOperationStatus = TEXT("Session opened");
	ToolData.Add(SessionId, MoveTemp(NewData));

	ClaireonAssetUtils::OpenAssetEditorIfHeadless(BB);

	FString StructureText = ClaireonBehaviorTreeHelpers::FormatBlackboardData(BB, false);

	TSharedPtr<FJsonObject> OpenData = MakeShared<FJsonObject>();
	OpenData->SetStringField(TEXT("session_id"), SessionId);
	OpenData->SetStringField(TEXT("asset_path"), AssetPath);
	OpenData->SetStringField(TEXT("status"), TEXT("Session opened"));
	OpenData->SetStringField(TEXT("structure"), StructureText);

	return MakeSuccessResult(OpenData, FString::Printf(TEXT("Opened session for %s"), *FPaths::GetBaseFilename(AssetPath)));
}
