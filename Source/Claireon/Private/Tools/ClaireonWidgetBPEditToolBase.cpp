// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPEditToolBase.h"
#include "Tools/ClaireonAssetUtils.h"
#include "ClaireonLog.h"
#include "ClaireonWidgetHelpers.h"
#include "ClaireonSessionManager.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"

#define LOCTEXT_NAMESPACE "ClaireonWidgetBPEditToolBase"

using FToolResult = IClaireonTool::FToolResult;

TMap<FString, FWidgetBPEditToolData> ClaireonWidgetBPEditToolBase::ToolData;
bool ClaireonWidgetBPEditToolBase::bDelegateRegistered = false;

// ============================================================================
// Session Delegate
// ============================================================================

void ClaireonWidgetBPEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == TEXT("widgetbp_edit"))
	{
		ToolData.Remove(Info.SessionId);
	}
}

// ============================================================================
// Session Helper
// ============================================================================

bool ClaireonWidgetBPEditToolBase::BeginSessionOp(
	const TSharedPtr<FJsonObject>& Arguments,
	const FString& OperationName,
	TSharedPtr<FJsonObject>& OutParams,
	FString& OutSessionId,
	FWidgetBPEditToolData*& OutData,
	FToolResult& OutError)
{
	// Unwrap legacy nested "params" object so both flat and envelope callers work.
	TSharedPtr<FJsonObject> Params = Arguments.IsValid() ? Arguments : MakeShared<FJsonObject>();
	if (Params->HasField(TEXT("params")))
	{
		const TSharedPtr<FJsonObject>* NestedObj = nullptr;
		if (Params->TryGetObjectField(TEXT("params"), NestedObj) && NestedObj && NestedObj->IsValid())
		{
			Params = *NestedObj;
		}
	}

	// Resolve session_id from either top-level Arguments or Params.
	FString SessionId;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
	{
		if (!Params->TryGetStringField(TEXT("session_id"), SessionId) || SessionId.IsEmpty())
		{
			OutError = MakeErrorResult(FString::Printf(TEXT("Operation '%s' requires session_id"), *OperationName));
			return false;
		}
	}

	FMCPSession* Session = FClaireonSessionManager::Get().FindSession(SessionId);
	if (!Session)
	{
		OutError = MakeErrorResult(FString::Printf(TEXT("Session not found or expired: %s"), *SessionId));
		return false;
	}

	FWidgetBPEditToolData* Data = ToolData.Find(SessionId);
	if (!Data)
	{
		OutError = MakeErrorResult(TEXT("Session tool data not found"));
		return false;
	}

	FClaireonSessionManager::Get().TouchSession(SessionId);

	OutParams = Params;
	OutSessionId = SessionId;
	OutData = Data;
	return true;
}

// ============================================================================
// State Response Helper
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::BuildStateResponse(const FString& SessionId, FWidgetBPEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Invalid session"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();

	// Serialize widget tree with default options
	FWidgetSerializeOptions Options;
	Options.bIncludeProperties = false;
	Options.MaxDepth = -1;
	TSharedPtr<FJsonObject> TreeData = ClaireonWidgetHelpers::SerializeWidgetTree(WBP, Options);

	// Build response JSON
	TSharedPtr<FJsonObject> ResponseObj = MakeShared<FJsonObject>();
	ResponseObj->SetStringField(TEXT("session_id"), SessionId);
	ResponseObj->SetStringField(TEXT("asset_path"), WBP->GetPathName());
	ResponseObj->SetStringField(TEXT("focused_widget"), Data->FocusedWidget.ToString());
	ResponseObj->SetBoolField(TEXT("modified"), Data->bModified);

	if (TreeData.IsValid())
	{
		ResponseObj->SetObjectField(TEXT("widget_tree"), TreeData);
	}

	FString SessionHintSummaryTag;
	ClaireonAssetUtils::EmitSessionHintIfNeeded(ResponseObj, Data->ConsecutiveAssetPathCalls, WBP->GetPathName(), SessionId, SessionHintSummaryTag);

	FString ResponseString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResponseString);
	FJsonSerializer::Serialize(ResponseObj.ToSharedRef(), Writer);

	return MakeSuccessResult(ResponseObj, ResponseString + SessionHintSummaryTag);
}

#undef LOCTEXT_NAMESPACE
