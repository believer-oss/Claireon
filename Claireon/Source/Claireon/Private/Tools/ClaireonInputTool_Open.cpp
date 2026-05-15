// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_Open.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "ClaireonSessionManager.h"
#include "InputAction.h"
#include "InputMappingContext.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_Open::GetName() const
{
	return TEXT("claireon.input_open");
}

FString ClaireonInputTool_Open::GetDescription() const
{
	return TEXT("Open an edit session on an existing Input Action or Input Mapping Context asset.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_Open::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("asset_path"), TEXT("Object path to the Input Action or Input Mapping Context."), true);
	Builder.AddBoolean(TEXT("suppress_output"), TEXT("Return only a brief status."));
	return Builder.Build();
}

FToolResult ClaireonInputTool_Open::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString AssetPath;
	if (!Arguments->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
	{
		return MakeErrorResult(TEXT("'open' requires 'asset_path'"));
	}

	FString LoadError;
	UObject* Asset = ClaireonEnhancedInputHelpers::LoadInputAsset(AssetPath, LoadError);
	if (!Asset)
	{
		return MakeErrorResult(LoadError);
	}

	EnsureDelegateRegistered();

	const FString ResolvedAssetPath = Asset->GetPathName();
	FMCPOpenSessionResult OpenResult = FClaireonSessionManager::Get().OpenSession(ResolvedAssetPath, InputSessionToolName);
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

	FInputEditToolData NewData;
	NewData.LastOperationStatus = TEXT("Session opened");

	if (UInputAction* IA = Cast<UInputAction>(Asset))
	{
		NewData.AssetType = EInputAssetType::InputAction;
		NewData.InputAction = IA;
	}
	else if (UInputMappingContext* IMC = Cast<UInputMappingContext>(Asset))
	{
		NewData.AssetType = EInputAssetType::MappingContext;
		NewData.MappingContext = IMC;
	}

	if (Arguments->HasField(TEXT("suppress_output")))
	{
		NewData.bSuppressOutput = Arguments->GetBoolField(TEXT("suppress_output"));
	}

	ToolData.Add(SessionId, MoveTemp(NewData));

	FInputEditToolData* Data = ToolData.Find(SessionId);
	return BuildStateResponse(SessionId, Data);
}
