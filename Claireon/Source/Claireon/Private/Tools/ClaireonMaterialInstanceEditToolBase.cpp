// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialInstanceEditToolBase.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "ClaireonSessionManager.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonMaterialInstanceEditToolBase::MaterialInstanceSessionToolName = TEXT("claireon.material_instance_edit");
TMap<FString, FMaterialInstanceEditToolData> ClaireonMaterialInstanceEditToolBase::ToolData;
bool ClaireonMaterialInstanceEditToolBase::bDelegateRegistered = false;

void ClaireonMaterialInstanceEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == MaterialInstanceSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

void ClaireonMaterialInstanceEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonMaterialInstanceEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonMaterialInstanceEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FMaterialInstanceEditToolData*& OutData,
	FString& OutError)
{
	if (!Arguments->TryGetStringField(TEXT("session_id"), OutSessionId) || OutSessionId.IsEmpty())
	{
		OutError = TEXT("Missing required parameter: session_id");
		return false;
	}

	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(OutSessionId);
	if (!Session)
	{
		OutError = FString::Printf(TEXT("Session not found or expired: %s"), *OutSessionId);
		return false;
	}

	OutData = ToolData.Find(OutSessionId);
	if (!OutData)
	{
		OutError = TEXT("Session tool data not found");
		return false;
	}

	if (!OutData->IsValid())
	{
		OutError = TEXT("MaterialInstance no longer valid. Reopen session.");
		return false;
	}

	bool bSuppressOutput = false;
	if (Arguments->HasField(TEXT("suppress_output")))
	{
		bSuppressOutput = Arguments->GetBoolField(TEXT("suppress_output"));
	}
	OutData->bSuppressOutput = bSuppressOutput;

	return true;
}

// ============================================================================
// State response
// ============================================================================

FToolResult ClaireonMaterialInstanceEditToolBase::BuildStateResponse(const FString& SessionId, FMaterialInstanceEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	if (Data->bSuppressOutput)
	{
		const FString Brief = Data->LastOperationStatus.IsEmpty()
			? FString(TEXT("ok"))
			: FString::Printf(TEXT("ok: %s"), *Data->LastOperationStatus);
		TSharedPtr<FJsonObject> BriefData = MakeShared<FJsonObject>();
		BriefData->SetStringField(TEXT("session_id"), SessionId);
		BriefData->SetStringField(TEXT("status"), Brief);
		return MakeSuccessResult(BriefData, Brief);
	}

	UMaterialInstanceConstant* Instance = Data->Instance.Get();
	if (!Instance)
	{
		return MakeErrorResult(TEXT("MaterialInstance is no longer valid"));
	}

	FString Output;
	Output += TEXT("=== Session Status ===\n");
	Output += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	Output += FString::Printf(TEXT("Asset: %s\n"), *Instance->GetPathName());
	const FString ParentStr = Instance->Parent ? Instance->Parent->GetPathName() : FString(TEXT("(none)"));
	Output += FString::Printf(TEXT("Parent: %s\n"), *ParentStr);
	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);
	Output += FString::Printf(TEXT("Overrides: scalar=%d, vector=%d, texture=%d\n"),
		Instance->ScalarParameterValues.Num(),
		Instance->VectorParameterValues.Num(),
		Instance->TextureParameterValues.Num());
	Output += TEXT("\n");
	Output += ClaireonMaterialHelpers::FormatMaterialInstance(Instance);

	TSharedPtr<FJsonObject> RespData = MakeShared<FJsonObject>();
	RespData->SetStringField(TEXT("session_id"), SessionId);
	RespData->SetStringField(TEXT("asset_path"), Instance->GetPathName());
	RespData->SetStringField(TEXT("parent_path"), ParentStr);
	RespData->SetNumberField(TEXT("scalar_override_count"), Instance->ScalarParameterValues.Num());
	RespData->SetNumberField(TEXT("vector_override_count"), Instance->VectorParameterValues.Num());
	RespData->SetNumberField(TEXT("texture_override_count"), Instance->TextureParameterValues.Num());
	RespData->SetStringField(TEXT("last_operation_status"), Data->LastOperationStatus);

	return MakeSuccessResult(RespData, Output);
}
