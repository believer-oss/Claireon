// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonLevelSequenceEditToolBase.h"
#include "Tools/ClaireonSequenceHelpers.h"
#include "ClaireonSessionManager.h"
#include "LevelSequence.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonLevelSequenceEditToolBase::LevelSequenceSessionToolName = TEXT("claireon.level_sequence_open");
TMap<FString, FSequenceEditToolData> ClaireonLevelSequenceEditToolBase::ToolData;
bool ClaireonLevelSequenceEditToolBase::bDelegateRegistered = false;

void ClaireonLevelSequenceEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == LevelSequenceSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

void ClaireonLevelSequenceEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonLevelSequenceEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonLevelSequenceEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FSequenceEditToolData*& OutData,
	FString& OutError)
{
	if (!Arguments.IsValid())
	{
		OutError = TEXT("Missing arguments");
		return false;
	}
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

FToolResult ClaireonLevelSequenceEditToolBase::BuildStateResponse(const FString& SessionId, FSequenceEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	FString AssetPath;
	if (FMCPSession* Session = FClaireonSessionManager::Get().FindSession(SessionId))
	{
		AssetPath = Session->AssetPath;
	}

	TSharedPtr<FJsonObject> Response = MakeShared<FJsonObject>();
	Response->SetStringField(TEXT("session_id"), SessionId);
	Response->SetStringField(TEXT("asset_path"), AssetPath);
	Response->SetNumberField(TEXT("focused_binding"), Data->FocusedBindingIndex);
	Response->SetNumberField(TEXT("focused_track"), Data->FocusedTrackIndex);
	if (!Data->LastOperationStatus.IsEmpty())
	{
		Response->SetStringField(TEXT("status"), Data->LastOperationStatus);
	}

	if (!Data->bSuppressOutput)
	{
		const FString Structure = FClaireonSequenceHelpers::FormatSequenceStructure(
			Data->Sequence.Get(), /*bKeyframes=*/true, /*bSections=*/true);
		Response->SetStringField(TEXT("sequence_structure"), Structure);
	}

	const FString Summary = Data->LastOperationStatus.IsEmpty()
		? FString::Printf(TEXT("Session %s: %s"), *SessionId, *AssetPath)
		: Data->LastOperationStatus;
	return MakeSuccessResult(Response, Summary);
}
