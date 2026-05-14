// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputEditToolBase.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "ClaireonSessionManager.h"
#include "InputAction.h"
#include "InputMappingContext.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonInputEditToolBase::InputSessionToolName = TEXT("input_edit");
TMap<FString, FInputEditToolData> ClaireonInputEditToolBase::ToolData;
bool ClaireonInputEditToolBase::bDelegateRegistered = false;

void ClaireonInputEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == InputSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

void ClaireonInputEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonInputEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonInputEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FInputEditToolData*& OutData,
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
		OutError = TEXT("Session is invalid. Reopen session.");
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

UInputAction* ClaireonInputEditToolBase::RequireInputAction(FInputEditToolData* Data, FString& OutError) const
{
	if (!Data || !Data->IsValid())
	{
		OutError = TEXT("Session is invalid");
		return nullptr;
	}
	if (Data->AssetType != EInputAssetType::InputAction)
	{
		OutError = TEXT("This operation is only valid for Input Action sessions");
		return nullptr;
	}
	return Data->InputAction.Get();
}

UInputMappingContext* ClaireonInputEditToolBase::RequireMappingContext(FInputEditToolData* Data, FString& OutError) const
{
	if (!Data || !Data->IsValid())
	{
		OutError = TEXT("Session is invalid");
		return nullptr;
	}
	if (Data->AssetType != EInputAssetType::MappingContext)
	{
		OutError = TEXT("This operation is only valid for Input Mapping Context sessions");
		return nullptr;
	}
	return Data->MappingContext.Get();
}

// ============================================================================
// State response
// ============================================================================

FToolResult ClaireonInputEditToolBase::BuildStateResponse(const FString& SessionId, FInputEditToolData* Data)
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

	FString Output;
	Output += TEXT("=== Session Status ===\n");
	Output += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);

	TSharedPtr<FJsonObject> RespData = MakeShared<FJsonObject>();
	RespData->SetStringField(TEXT("session_id"), SessionId);
	RespData->SetStringField(TEXT("last_operation_status"), Data->LastOperationStatus);

	if (Data->AssetType == EInputAssetType::InputAction)
	{
		UInputAction* IA = Data->InputAction.Get();
		Output += TEXT("Asset Type: Input Action\n");
		Output += FString::Printf(TEXT("Asset: %s\n\n"), *IA->GetPathName());
		Output += ClaireonEnhancedInputHelpers::FormatInputAction(IA, false);

		RespData->SetStringField(TEXT("asset_type"), TEXT("input_action"));
		RespData->SetStringField(TEXT("asset_path"), IA->GetPathName());
	}
	else
	{
		UInputMappingContext* IMC = Data->MappingContext.Get();
		Output += TEXT("Asset Type: Input Mapping Context\n");
		Output += FString::Printf(TEXT("Asset: %s\n\n"), *IMC->GetPathName());
		Output += ClaireonEnhancedInputHelpers::FormatMappingContext(IMC, false);

		RespData->SetStringField(TEXT("asset_type"), TEXT("mapping_context"));
		RespData->SetStringField(TEXT("asset_path"), IMC->GetPathName());
	}

	return MakeSuccessResult(RespData, Output);
}
