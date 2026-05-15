// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBehaviorTreeEditToolBase.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonSessionManager.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTreeGraph.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonBehaviorTreeEditToolBase::BehaviorTreeSessionToolName = TEXT("claireon.behaviortree_edit");
TMap<FString, FBehaviorTreeEditToolData> ClaireonBehaviorTreeEditToolBase::ToolData;
bool ClaireonBehaviorTreeEditToolBase::bDelegateRegistered = false;

void ClaireonBehaviorTreeEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == BehaviorTreeSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

void ClaireonBehaviorTreeEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonBehaviorTreeEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

bool ClaireonBehaviorTreeEditToolBase::ParseGuidParam(const TSharedPtr<FJsonObject>& Params, const FString& FieldName, FGuid& OutGuid, FString& OutError)
{
	FString GuidStr;
	if (!Params->TryGetStringField(FieldName, GuidStr) || GuidStr.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Missing required parameter: %s"), *FieldName);
		return false;
	}
	if (!FGuid::Parse(GuidStr, OutGuid))
	{
		OutError = FString::Printf(TEXT("Invalid GUID for %s: %s"), *FieldName, *GuidStr);
		return false;
	}
	return true;
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonBehaviorTreeEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FBehaviorTreeEditToolData*& OutData,
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

FToolResult ClaireonBehaviorTreeEditToolBase::BuildStateResponse(const FString& SessionId, FBehaviorTreeEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

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

	FString Error;
	UBehaviorTreeGraph* Graph = ClaireonBehaviorTreeHelpers::GetBTGraph(Data->BehaviorTree.Get(), Error);
	if (!Graph)
	{
		return MakeErrorResult(FString::Printf(TEXT("Session error: %s"), *Error));
	}

	FString Output;
	Output += TEXT("=== Session Status ===\n");
	Output += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	Output += FString::Printf(TEXT("Asset: %s\n"), *Data->BehaviorTree->GetPathName());
	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);
	Output += TEXT("\n");
	Output += ClaireonBehaviorTreeHelpers::FormatBTGraphStructure(Graph, false);

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("asset_path"), Data->BehaviorTree->GetPathName());
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("last_operation"), Data->LastOperationStatus);
	ResponseData->SetStringField(TEXT("tree_view"), Output);

	const FString Summary = FString::Printf(TEXT("Session %s: %s"),
		*SessionId.Left(8), *Data->LastOperationStatus);

	return MakeSuccessResult(ResponseData, Summary);
}
