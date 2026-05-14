// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraTool_Open.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonPathResolver.h"
#include "ClaireonSessionManager.h"
#include "NiagaraSystem.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonNiagaraTool_Open::GetName() const
{
	return TEXT("claireon.niagara_open");
}

TArray<FString> ClaireonNiagaraTool_Open::GetSearchKeywords() const
{
	return {TEXT("niagara"), TEXT("niag"), TEXT("vfx"), TEXT("fx"), TEXT("particles"), TEXT("effect"), TEXT("emitter"), TEXT("open")};
}

FString ClaireonNiagaraTool_Open::GetDescription() const
{
	return TEXT("Open a Niagara System asset for editing. Returns a session_id for subsequent operations.");
}

TSharedPtr<FJsonObject> ClaireonNiagaraTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Object path to the Niagara System asset to edit."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("Return only a brief status."));
	return Builder.Build();
}

FToolResult ClaireonNiagaraTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'open' requires 'asset_path'"));
	}

	auto ResolveResult = ClaireonPathResolver::Resolve(AssetPath);
	if (!ResolveResult.bSuccess)
	{
		return MakeErrorResult(ResolveResult.Error);
	}
	AssetPath = ResolveResult.ResolvedPath.Path;

	FString Error;
	UNiagaraSystem* System = ClaireonNiagaraHelpers::LoadNiagaraSystemAsset(AssetPath, Error);
	if (!System)
	{
		return MakeErrorResult(Error);
	}

	EnsureDelegateRegistered();

	const FString ResolvedAssetPath = System->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, NiagaraSessionToolName);
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

	FNiagaraEditToolData NewData;
	NewData.System = System;
	NewData.LastOperationStatus = TEXT("Session opened");
	if (Arguments->HasField(TEXT("suppress_output")))
	{
		NewData.bSuppressOutput = Arguments->GetBoolField(TEXT("suppress_output"));
	}
	ToolData.Add(SessionId, MoveTemp(NewData));

	FNiagaraEditToolData* Data = ToolData.Find(SessionId);
	return BuildStateResponse(SessionId, Data);
}
