// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_Save.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonSafeExec.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "UObject/Package.h"
#include "FileHelpers.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_Save::GetOperation() const { return TEXT("save"); }

FString ClaireonInputTool_Save::GetDescription() const
{
    return TEXT("Save the asset owned by the Input edit session to disk. Session-mode tool: open via input_open first.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_Save::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	return Builder.Build();
}

FToolResult ClaireonInputTool_Save::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FInputEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	UObject* Asset = (Data->AssetType == EInputAssetType::InputAction)
		? static_cast<UObject*>(Data->InputAction.Get())
		: static_cast<UObject*>(Data->MappingContext.Get());

	if (!Asset)
	{
		return MakeErrorResult(TEXT("Asset is no longer valid"));
	}

	UPackage* Package = Asset->GetPackage();
	Package->SetDirtyFlag(true);

	TArray<UPackage*> PackagesToSave;
	PackagesToSave.Add(Package);
	if (ClaireonSafeExec::DidLastExecutionCrash())
	{
		return MakeErrorResult(TEXT("Save blocked: editor state may be corrupted after a previous crash. Restart the editor."));
	}
	const bool bSuccess = UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);

	if (bSuccess)
	{
		Data->LastOperationStatus = FString::Printf(TEXT("save -- Saved %s"), *Asset->GetPathName());
		TSharedPtr<FJsonObject> RespData = MakeShared<FJsonObject>();
		RespData->SetStringField(TEXT("session_id"), SessionId);
		RespData->SetStringField(TEXT("asset_path"), Asset->GetPathName());
		RespData->SetBoolField(TEXT("saved"), true);
		return MakeSuccessResult(RespData, FString::Printf(TEXT("Saved: %s"), *Asset->GetPathName()));
	}

	Data->LastOperationStatus = TEXT("save -- Failed");
	return MakeErrorResult(TEXT("Failed to save package"));
}
