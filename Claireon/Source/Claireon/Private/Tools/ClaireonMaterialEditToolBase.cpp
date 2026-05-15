// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonMaterialEditToolBase.h"
#include "Tools/ClaireonMaterialHelpers.h"
#include "ClaireonSessionManager.h"
#include "Materials/Material.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonMaterialEditToolBase::MaterialSessionToolName = TEXT("claireon.material_edit");
TMap<FString, FMaterialEditToolData> ClaireonMaterialEditToolBase::ToolData;
bool ClaireonMaterialEditToolBase::bDelegateRegistered = false;

void ClaireonMaterialEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == MaterialSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

void ClaireonMaterialEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonMaterialEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonMaterialEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FMaterialEditToolData*& OutData,
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
		OutError = TEXT("Material no longer valid. Reopen session.");
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

FToolResult ClaireonMaterialEditToolBase::BuildStateResponse(const FString& SessionId, FMaterialEditToolData* Data)
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

	UMaterial* Material = Data->Material.Get();
	if (!Material)
	{
		return MakeErrorResult(TEXT("Material is no longer valid"));
	}

	FString Output;
	Output += TEXT("=== Session Status ===\n");
	Output += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	Output += FString::Printf(TEXT("Asset: %s\n"), *Material->GetPathName());
	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);
	Output += TEXT("\n");
	Output += ClaireonMaterialHelpers::FormatMaterialStructure(Material, TEXT("summary"));

	TSharedPtr<FJsonObject> RespData = MakeShared<FJsonObject>();
	RespData->SetStringField(TEXT("session_id"), SessionId);
	RespData->SetStringField(TEXT("asset_path"), Material->GetPathName());
	RespData->SetNumberField(TEXT("expression_count"), Material->GetExpressions().Num());
	RespData->SetStringField(TEXT("last_operation_status"), Data->LastOperationStatus);

	return MakeSuccessResult(RespData, Output);
}
