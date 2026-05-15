// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonNiagaraEditToolBase.h"
#include "Tools/ClaireonNiagaraHelpers.h"
#include "ClaireonSessionManager.h"
#include "NiagaraSystem.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonNiagaraEditToolBase::NiagaraSessionToolName = TEXT("claireon.niagara_edit");
TMap<FString, FNiagaraEditToolData> ClaireonNiagaraEditToolBase::ToolData;
bool ClaireonNiagaraEditToolBase::bDelegateRegistered = false;

void ClaireonNiagaraEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == NiagaraSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

void ClaireonNiagaraEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonNiagaraEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonNiagaraEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FNiagaraEditToolData*& OutData,
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

// ============================================================================
// State response
// ============================================================================

FToolResult ClaireonNiagaraEditToolBase::BuildStateResponse(const FString& SessionId, FNiagaraEditToolData* Data)
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

	UNiagaraSystem* System = Data->System.Get();

	FString Output;
	Output += TEXT("=== Session Status ===\n");
	Output += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	Output += FString::Printf(TEXT("Asset: %s\n"), *System->GetPathName());
	Output += FString::Printf(TEXT("Focused Emitter: %s\n"),
		Data->FocusedEmitterIndex < 0 ? TEXT("System Level") : *FString::Printf(TEXT("%d"), Data->FocusedEmitterIndex));
	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);
	Output += TEXT("\n");
	Output += ClaireonNiagaraHelpers::FormatNiagaraSystemStructure(System, false);

	TSharedPtr<FJsonObject> RespData = MakeShared<FJsonObject>();
	RespData->SetStringField(TEXT("session_id"), SessionId);
	RespData->SetStringField(TEXT("asset_path"), System->GetPathName());
	RespData->SetStringField(TEXT("last_operation_status"), Data->LastOperationStatus);
	RespData->SetNumberField(TEXT("focused_emitter_index"), Data->FocusedEmitterIndex);

	return MakeSuccessResult(RespData, Output);
}
