// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonEQSTool_Open.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "Misc/Paths.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonEQSTool_Open::GetName() const
{
	return TEXT("claireon.eqs_open");
}

TArray<FString> ClaireonEQSTool_Open::GetSearchKeywords() const
{
	return {TEXT("eqs"), TEXT("environment"), TEXT("query"), TEXT("system"), TEXT("ai"), TEXT("open"), TEXT("session")};
}

FString ClaireonEQSTool_Open::GetDescription() const
{
	return TEXT("Open an EQS Query asset for editing. Returns a session_id for subsequent operations.");
}

TSharedPtr<FJsonObject> ClaireonEQSTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the EQS Query asset to edit."), true);
	return Builder.Build();
}

FToolResult ClaireonEQSTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
	UEnvQuery* Query = ClaireonBehaviorTreeHelpers::LoadEQSAsset(AssetPath, Error);
	if (!Query)
	{
		return MakeErrorResult(Error);
	}

	EnsureDelegateRegistered();

	const FString ResolvedAssetPath = Query->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, EQSSessionToolName);
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

	FEQSEditToolData NewData;
	NewData.Query = Query;
	NewData.LastOperationStatus = TEXT("Session opened");
	ToolData.Add(SessionId, MoveTemp(NewData));

	FString StructureText = ClaireonBehaviorTreeHelpers::FormatEQSStructure(Query, false);

	TSharedPtr<FJsonObject> OpenData = MakeShared<FJsonObject>();
	OpenData->SetStringField(TEXT("session_id"), SessionId);
	OpenData->SetStringField(TEXT("asset_path"), AssetPath);
	OpenData->SetStringField(TEXT("status"), TEXT("Session opened"));
	OpenData->SetStringField(TEXT("structure"), StructureText);

	return MakeSuccessResult(OpenData, FString::Printf(TEXT("Opened session for %s"), *FPaths::GetBaseFilename(AssetPath)));
}
