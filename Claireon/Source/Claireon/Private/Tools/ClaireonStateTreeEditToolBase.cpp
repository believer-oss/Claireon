// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonStateTreeEditToolBase.h"
#include "Tools/ClaireonStateTreeEditInternal.h"
#include "Tools/ClaireonStateTreeHelpers.h"
#include "ClaireonSessionManager.h"
#include "StateTree.h"
#include "StateTreeEditorData.h"
#include "StateTreeState.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonStateTreeEditToolBase::StateTreeSessionToolName = TEXT("claireon.statetree_edit");
TMap<FString, FStateTreeEditToolData> ClaireonStateTreeEditToolBase::ToolData;
bool ClaireonStateTreeEditToolBase::bDelegateRegistered = false;

void ClaireonStateTreeEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == StateTreeSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

void ClaireonStateTreeEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonStateTreeEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonStateTreeEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FStateTreeEditToolData*& OutData,
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

FToolResult ClaireonStateTreeEditToolBase::BuildStateResponse(
	const FString& SessionId,
	FStateTreeEditToolData* Data,
	FStringView ExtraField,
	FStringView ExtraValue)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	if (Data->bSuppressOutput)
	{
		const FString StatusMsg = Data->LastOperationStatus.IsEmpty()
			? FString(TEXT("ok"))
			: FString::Printf(TEXT("ok: %s"), *Data->LastOperationStatus);
		TSharedPtr<FJsonObject> SuppressData = MakeShared<FJsonObject>();
		SuppressData->SetStringField(TEXT("session_id"), SessionId);
		SuppressData->SetStringField(TEXT("status"), StatusMsg);
		if (!ExtraField.IsEmpty() && !ExtraValue.IsEmpty())
		{
			SuppressData->SetStringField(FString(ExtraField), FString(ExtraValue));
		}
		return MakeSuccessResult(SuppressData, StatusMsg);
	}

	FString Error;
	UStateTreeEditorData* EditorData = ClaireonStateTreeHelpers::GetEditorData(Data->StateTree.Get(), Error);
	if (!EditorData)
	{
		return MakeErrorResult(FString::Printf(TEXT("Session error: %s"), *Error));
	}

	FString Output;
	Output += TEXT("=== Session Status ===\n");
	Output += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	Output += FString::Printf(TEXT("Asset: %s\n"), *Data->StateTree->GetPathName());

	if (Data->FocusedStateId.IsValid())
	{
		UStateTreeState* FocusState = ClaireonStateTreeHelpers::FindStateById(EditorData, Data->FocusedStateId);
		if (FocusState)
		{
			Output += FString::Printf(TEXT("Focused State: [%s] %s\n"),
				*Data->FocusedStateId.ToString(EGuidFormats::DigitsWithHyphensLower),
				*FocusState->Name.ToString());
		}
	}

	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);
	Output += TEXT("\n");

	// Show affected area (focused state)
	if (Data->FocusedStateId.IsValid())
	{
		UStateTreeState* FocusState = ClaireonStateTreeHelpers::FindStateById(EditorData, Data->FocusedStateId);
		if (FocusState)
		{
			Output += TEXT("=== Affected Area ===\n");
			Output += ClaireonStateTreeHelpers::FormatStateArea(FocusState);
		}
	}

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("asset_path"), Data->StateTree->GetPathName());
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("last_operation"), Data->LastOperationStatus);
	ClaireonStateTreeEditInternal::ApplyStructuredSpill(
		*ResponseData, TEXT("state_view"), TEXT("state_view_full"), Output);

	if (!ExtraField.IsEmpty() && !ExtraValue.IsEmpty())
	{
		ResponseData->SetStringField(FString(ExtraField), FString(ExtraValue));
	}

	const FString Summary = FString::Printf(TEXT("Session %s: %s"),
		*SessionId.Left(8), *Data->LastOperationStatus);

	return MakeSuccessResult(ResponseData, Summary);
}
