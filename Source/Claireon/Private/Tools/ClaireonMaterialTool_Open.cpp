// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialTool_Open.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonSessionManager.h"
#include "Materials/Material.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialTool_Open::GetOperation() const { return TEXT("open"); }

FString ClaireonMaterialTool_Open::GetDescription() const
{
    return TEXT("Open a UMaterial asset for session-based editing (acquires a lock and returns a session_id so subsequent edits run in-session).");
}

TSharedPtr<FJsonObject> ClaireonMaterialTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the Material asset to open."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("Return only a brief status instead of full state."));
	return Builder.Build();
}

FToolResult ClaireonMaterialTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'open' requires 'asset_path'"));
	}

	FString LoadErr;
	UMaterial* Material = ClaireonMaterialHelpers::LoadMaterialAsset(AssetPath, LoadErr);
	if (!Material)
	{
		return MakeErrorResult(LoadErr);
	}

	EnsureDelegateRegistered();

	const FString ResolvedPath = Material->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedPath, MaterialSessionToolName);
	if (OpenResult.Result == EOpenSessionResult::BlockedByOtherTool)
	{
		const FMCPSession& Blocker = OpenResult.BlockingSession.GetValue();
		return MakeErrorResult(FString::Printf(TEXT("Asset is locked by %s session %s"), *Blocker.ToolName, *Blocker.SessionId));
	}
	if (OpenResult.Result == EOpenSessionResult::InvalidAssetPath)
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid asset path: %s"), *ResolvedPath));
	}

	const FString SessionId = OpenResult.SessionId;

	FMaterialEditToolData NewData;
	NewData.Material = Material;
	NewData.LastOperationStatus = TEXT("Session opened");
	if (Arguments->HasField(TEXT("suppress_output")))
	{
		NewData.bSuppressOutput = Arguments->GetBoolField(TEXT("suppress_output"));
	}
	ToolData.Add(SessionId, MoveTemp(NewData));

	ClaireonAssetUtils::OpenAssetEditorIfHeadless(Material);

	FMaterialEditToolData* Data = ToolData.Find(SessionId);
	return BuildStateResponse(SessionId, Data);
}
