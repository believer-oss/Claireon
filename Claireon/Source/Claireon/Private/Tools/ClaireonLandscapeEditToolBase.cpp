// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLandscapeEditToolBase.h"
#include "Tools/ClaireonLandscapeHelpers.h"
#include "ClaireonSessionManager.h"
#include "LandscapeProxy.h"
#include "LandscapeInfo.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonLandscapeEditToolBase::LandscapeSessionToolName = TEXT("landscape_edit");
TMap<FString, FLandscapeEditToolData> ClaireonLandscapeEditToolBase::ToolData;
bool ClaireonLandscapeEditToolBase::bDelegateRegistered = false;

void ClaireonLandscapeEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == LandscapeSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

void ClaireonLandscapeEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonLandscapeEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonLandscapeEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FLandscapeEditToolData*& OutData,
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
		OutError = TEXT("Landscape no longer valid. Close and reopen session.");
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

FToolResult ClaireonLandscapeEditToolBase::BuildStateResponse(const FString& SessionId, FLandscapeEditToolData* Data)
{
	if (Data->bSuppressOutput)
	{
		TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
		ResultData->SetStringField(TEXT("session_id"), SessionId);
		ResultData->SetStringField(TEXT("status"), Data->LastOperationStatus);
		return MakeSuccessResult(ResultData, Data->LastOperationStatus);
	}

	TSharedPtr<FJsonObject> LandscapeJson = ClaireonLandscapeHelpers::BuildLandscapeInfoJson(
		Data->LandscapeInfo.Get(), TEXT("full"));

	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("session_id"), SessionId);
	ResultData->SetStringField(TEXT("status"), Data->LastOperationStatus);
	if (LandscapeJson)
	{
		ResultData->SetObjectField(TEXT("landscape"), LandscapeJson);
	}

	const FString Summary = FString::Printf(
		TEXT("Session %s: %s"), *SessionId, *Data->LastOperationStatus);
	return MakeSuccessResult(ResultData, Summary);
}
