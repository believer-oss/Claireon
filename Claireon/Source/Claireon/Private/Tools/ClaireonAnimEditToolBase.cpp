// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonAnimEditToolBase.h"
#include "Tools/ClaireonAnimHelpers.h"
#include "ClaireonSessionManager.h"
#include "ClaireonLog.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimMontage.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonAnimEditToolBase::AnimSessionToolName = TEXT("anim");
TMap<FString, FAnimEditToolData> ClaireonAnimEditToolBase::ToolData;
bool ClaireonAnimEditToolBase::bDelegateRegistered = false;

void ClaireonAnimEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == AnimSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

void ClaireonAnimEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonAnimEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonAnimEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FAnimEditToolData*& OutData,
	FToolResult& OutError)
{
	if (!Arguments->TryGetStringField(TEXT("session_id"), OutSessionId) || OutSessionId.IsEmpty())
	{
		OutError = MakeErrorResult(TEXT("Missing required parameter: session_id"));
		return false;
	}

	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(OutSessionId);
	if (!Session)
	{
		OutError = MakeErrorResult(FString::Printf(TEXT("Session not found or expired: %s"), *OutSessionId));
		return false;
	}
	Session->Touch();

	OutData = ToolData.Find(OutSessionId);
	if (!OutData || !OutData->IsValid())
	{
		OutError = MakeErrorResult(TEXT("Session tool data not found or animation asset was unloaded"));
		return false;
	}

	// Handle suppress_output
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

FToolResult ClaireonAnimEditToolBase::BuildStateResponse(const FString& SessionId, FAnimEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	// Fast path: skip expensive animation state output when caller doesn't need it
	if (Data->bSuppressOutput)
	{
		const FString StatusMsg = Data->LastOperationStatus.IsEmpty()
			? TEXT("ok")
			: FString::Printf(TEXT("ok: %s"), *Data->LastOperationStatus);
		TSharedPtr<FJsonObject> SuppressData = MakeShared<FJsonObject>();
		SuppressData->SetStringField(TEXT("session_id"), SessionId);
		SuppressData->SetStringField(TEXT("status"), StatusMsg);
		return MakeSuccessResult(SuppressData, StatusMsg);
	}

	// Full response
	FString Output;
	Output += TEXT("=== Session Status ===\n");
	Output += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	Output += FString::Printf(TEXT("Asset: %s (%s)\n"), *Data->Animation->GetPathName(), *Data->AssetType);
	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);

	if (Data->FocusedNotifyIndex >= 0)
	{
		Output += FString::Printf(TEXT("Focused Notify: [%d]\n"), Data->FocusedNotifyIndex);
		Output += TEXT("\n");
		Output += ClaireonAnimHelpers::FormatSingleNotify(Data->Animation.Get(), Data->FocusedNotifyIndex);
	}

	Output += TEXT("\n");

	// Show animation structure (compact)
	Output += ClaireonAnimHelpers::FormatAnimStructure(Data->Animation.Get(), Data->AssetType, false);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("asset_path"), Data->Animation->GetPathName());
	ResponseData->SetStringField(TEXT("asset_type"), Data->AssetType);
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("last_operation"), Data->LastOperationStatus);
	ResponseData->SetStringField(TEXT("state_view"), Output);

	const FString Summary = FString::Printf(TEXT("Session %s: %s"),
		*SessionId.Left(8), *Data->LastOperationStatus);

	return MakeSuccessResult(ResponseData, Summary);
}

// ============================================================================
// Type requirement helpers
// ============================================================================

UAnimMontage* ClaireonAnimEditToolBase::RequireMontage(FAnimEditToolData* Data, FToolResult& OutError)
{
	if (Data->AssetType != TEXT("AnimMontage"))
	{
		OutError = MakeErrorResult(TEXT("This operation is only valid for AnimMontage assets"));
		return nullptr;
	}
	UAnimMontage* Montage = Cast<UAnimMontage>(Data->Animation.Get());
	if (!Montage)
	{
		OutError = MakeErrorResult(TEXT("Failed to cast to AnimMontage"));
	}
	return Montage;
}

UAnimSequence* ClaireonAnimEditToolBase::RequireAnimSequence(FAnimEditToolData* Data, FToolResult& OutError)
{
	if (Data->AssetType != TEXT("AnimSequence"))
	{
		OutError = MakeErrorResult(TEXT("This operation is only valid for AnimSequence assets"));
		return nullptr;
	}
	UAnimSequence* AnimSeq = Cast<UAnimSequence>(Data->Animation.Get());
	if (!AnimSeq)
	{
		OutError = MakeErrorResult(TEXT("Failed to cast to AnimSequence"));
	}
	return AnimSeq;
}

// FToolSchemaBuilder definitions now live in Private/Tools/FToolSchemaBuilder.cpp.
