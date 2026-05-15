// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonPCGGraphEditToolBase.h"
#include "Tools/ClaireonPCGGraphHelpers.h"
#include "ClaireonSessionManager.h"
#include "PCGGraph.h"
#include "PCGNode.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonPCGGraphEditToolBase::PCGSessionToolName = TEXT("claireon.pcg_edit");
TMap<FString, FPCGGraphEditToolData> ClaireonPCGGraphEditToolBase::ToolData;
bool ClaireonPCGGraphEditToolBase::bDelegateRegistered = false;

void ClaireonPCGGraphEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == PCGSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

void ClaireonPCGGraphEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonPCGGraphEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonPCGGraphEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FPCGGraphEditToolData*& OutData,
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

FToolResult ClaireonPCGGraphEditToolBase::BuildStateResponse(const FString& SessionId, FPCGGraphEditToolData* Data)
{
	if (!Data || !Data->IsValid())
	{
		return MakeErrorResult(TEXT("Session is invalid"));
	}

	// Get asset path from the session manager
	FString AssetPath;
	if (FMCPSession* Session = FClaireonSessionManager::Get().FindSession(SessionId))
	{
		AssetPath = Session->AssetPath;
	}

	TSharedPtr<FJsonObject> ResponseData = MakeShared<FJsonObject>();
	ResponseData->SetStringField(TEXT("session_id"), SessionId);
	ResponseData->SetStringField(TEXT("asset_path"), AssetPath);

	if (!Data->LastOperationStatus.IsEmpty())
	{
		ResponseData->SetStringField(TEXT("status"), Data->LastOperationStatus);
	}

	if (!Data->bSuppressOutput)
	{
		FString GraphStructure = ClaireonPCGGraphHelpers::FormatGraphStructure(Data->PCGGraph.Get(), TEXT("summary"));
		ResponseData->SetStringField(TEXT("graph_structure"), GraphStructure);

		if (Data->FocusedNodeIndex != INDEX_NONE)
		{
			const TArray<UPCGNode*>& Nodes = Data->PCGGraph->GetNodes();
			if (Data->FocusedNodeIndex >= 0 && Data->FocusedNodeIndex < Nodes.Num())
			{
				FString FocusedDetail = ClaireonPCGGraphHelpers::FormatNodeDetail(
					Data->PCGGraph.Get(), Nodes[Data->FocusedNodeIndex], Data->FocusedNodeIndex, true);
				ResponseData->SetStringField(TEXT("focused_node"), FocusedDetail);
			}
		}
	}

	FString Summary = Data->LastOperationStatus.IsEmpty()
		? FString::Printf(TEXT("Session %s: %s"), *SessionId, *AssetPath)
		: Data->LastOperationStatus;

	return MakeSuccessResult(ResponseData, Summary);
}
