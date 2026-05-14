// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeTool_Open.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"
#include "Misc/Paths.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonBehaviorTreeTool_Open::GetName() const
{
	return TEXT("claireon.behaviortree_open");
}

TArray<FString> ClaireonBehaviorTreeTool_Open::GetSearchKeywords() const
{
	return {TEXT("bt"), TEXT("ai"), TEXT("behavior"), TEXT("tree"), TEXT("behaviortree"), TEXT("open"), TEXT("session")};
}

FString ClaireonBehaviorTreeTool_Open::GetDescription() const
{
	return TEXT("Open a Behavior Tree asset for editing. Returns a session_id for subsequent operations.");
}

TSharedPtr<FJsonObject> ClaireonBehaviorTreeTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the Behavior Tree asset to edit."), true);
	return Builder.Build();
}

FToolResult ClaireonBehaviorTreeTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	UBehaviorTree* BT = ClaireonBehaviorTreeHelpers::LoadBehaviorTreeAsset(AssetPath, Error);
	if (!BT)
	{
		return MakeErrorResult(Error);
	}

	UBehaviorTreeGraph* BTGraph = ClaireonBehaviorTreeHelpers::GetBTGraph(BT, Error);
	if (!BTGraph)
	{
		return MakeErrorResult(Error);
	}

	EnsureDelegateRegistered();

	const FString ResolvedAssetPath = BT->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, BehaviorTreeSessionToolName);
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

	FBehaviorTreeEditToolData NewData;
	NewData.BehaviorTree = BT;
	NewData.BTGraph = BTGraph;
	NewData.LastOperationStatus = TEXT("Session opened");
	ToolData.Add(SessionId, MoveTemp(NewData));

	FString StructureText = ClaireonBehaviorTreeHelpers::FormatBTGraphStructure(BTGraph, false);

	TSharedPtr<FJsonObject> OpenData = MakeShared<FJsonObject>();
	OpenData->SetStringField(TEXT("session_id"), SessionId);
	OpenData->SetStringField(TEXT("asset_path"), AssetPath);
	OpenData->SetStringField(TEXT("status"), TEXT("Session opened"));
	OpenData->SetStringField(TEXT("structure"), StructureText);

	return MakeSuccessResult(OpenData, FString::Printf(TEXT("Opened session for %s"), *FPaths::GetBaseFilename(AssetPath)));
}
