// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonFoliageEditToolBase.h"
#include "ClaireonSessionManager.h"
#include "InstancedFoliageActor.h"
#include "InstancedFoliage.h"
#include "FoliageType.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonFoliageEditToolBase::FoliageSessionToolName = TEXT("claireon.foliage_edit");
TMap<FString, FFoliageEditToolData> ClaireonFoliageEditToolBase::ToolData;
bool ClaireonFoliageEditToolBase::bDelegateRegistered = false;

void ClaireonFoliageEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == FoliageSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

void ClaireonFoliageEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonFoliageEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

UFoliageType* ClaireonFoliageEditToolBase::FindFoliageTypeInActor(AInstancedFoliageActor* IFA, const FString& NameOrPath)
{
#if WITH_EDITOR
	if (!IFA)
	{
		return nullptr;
	}

	for (auto& Pair : IFA->GetFoliageInfos())
	{
		UFoliageType* FT = Pair.Key;
		if (!FT) continue;

		if (FT->GetName().Equals(NameOrPath, ESearchCase::IgnoreCase) ||
			FT->GetPathName().Equals(NameOrPath, ESearchCase::IgnoreCase))
		{
			return FT;
		}
	}
#endif
	return nullptr;
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonFoliageEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FFoliageEditToolData*& OutData,
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

	if (!OutData->FoliageActor.IsValid())
	{
		OutError = TEXT("Foliage actor no longer valid. Reopen session.");
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

FToolResult ClaireonFoliageEditToolBase::BuildStateResponse(const FString& SessionId, FFoliageEditToolData* Data)
{
	TSharedPtr<FJsonObject> ResultData = MakeShared<FJsonObject>();
	ResultData->SetStringField(TEXT("session_id"), SessionId);
	ResultData->SetStringField(TEXT("status"), Data->LastOperationStatus);

	if (Data->bSuppressOutput)
	{
		return MakeSuccessResult(ResultData, Data->LastOperationStatus);
	}

#if WITH_EDITOR
	AInstancedFoliageActor* IFA = Data->FoliageActor.Get();

	TArray<TSharedPtr<FJsonValue>> TypeArray;
	int32 TotalInstances = 0;

	for (auto& Pair : IFA->GetFoliageInfos())
	{
		UFoliageType* FT = Pair.Key;
		const FFoliageInfo& Info = Pair.Value.Get();

		TSharedPtr<FJsonObject> TypeJson = MakeShared<FJsonObject>();
		TypeJson->SetStringField(TEXT("name"), FT ? FT->GetName() : TEXT("Unknown"));
		TypeJson->SetStringField(TEXT("asset_path"), FT ? FT->GetPathName() : TEXT(""));
		TypeJson->SetNumberField(TEXT("instance_count"), Info.Instances.Num());
		TypeArray.Add(MakeShared<FJsonValueObject>(TypeJson));
		TotalInstances += Info.Instances.Num();
	}

	ResultData->SetArrayField(TEXT("foliage_types"), TypeArray);
	ResultData->SetNumberField(TEXT("total_instances"), TotalInstances);
#endif

	const FString Summary = FString::Printf(
		TEXT("Session %s: %s"), *SessionId, *Data->LastOperationStatus);
	return MakeSuccessResult(ResultData, Summary);
}
