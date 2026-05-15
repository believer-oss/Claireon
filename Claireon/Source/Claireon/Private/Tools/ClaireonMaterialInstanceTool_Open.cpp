// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceTool_Open.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "ClaireonSessionManager.h"
#include "Materials/MaterialInstanceConstant.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonMaterialInstanceTool_Open::GetName() const
{
	return TEXT("claireon.material_instance_open");
}

FString ClaireonMaterialInstanceTool_Open::GetDescription() const
{
	return TEXT("Open an editing session on an existing UMaterialInstanceConstant asset. Returns a session_id for subsequent operations.");
}

TSharedPtr<FJsonObject> ClaireonMaterialInstanceTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Path to the UMaterialInstanceConstant asset to open."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("When true, response is brief status only."));
	return Builder.Build();
}

FToolResult ClaireonMaterialInstanceTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: asset_path"));
	}

	FString LoadErr;
	UMaterialInstanceConstant* Instance = ClaireonMaterialHelpers::LoadMaterialInstanceAsset(AssetPath, LoadErr);
	if (!Instance)
	{
		return MakeErrorResult(LoadErr);
	}

	EnsureDelegateRegistered();

	const FString ResolvedPath = Instance->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedPath, MaterialInstanceSessionToolName);
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

	FMaterialInstanceEditToolData NewData;
	NewData.Instance = Instance;
	NewData.LastOperationStatus = TEXT("Session opened");

	bool bSuppressOutput = false;
	Arguments->TryGetBoolField(TEXT("suppress_output"), bSuppressOutput);
	NewData.bSuppressOutput = bSuppressOutput;

	ToolData.Add(SessionId, MoveTemp(NewData));

	FMaterialInstanceEditToolData* Data = ToolData.Find(SessionId);
	return BuildStateResponse(SessionId, Data);
}
