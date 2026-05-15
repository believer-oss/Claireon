// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonBlackboardEditToolBase.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonSessionManager.h"
#include "BehaviorTree/BlackboardData.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Bool.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Int.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Float.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_String.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Name.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Vector.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Rotator.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Object.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Class.h"
#include "BehaviorTree/Blackboard/BlackboardKeyType_Enum.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonBlackboardEditToolBase::BlackboardSessionToolName = TEXT("claireon.blackboard_edit");
TMap<FString, FBlackboardEditToolData> ClaireonBlackboardEditToolBase::ToolData;
bool ClaireonBlackboardEditToolBase::bDelegateRegistered = false;

void ClaireonBlackboardEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == BlackboardSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

UBlackboardKeyType* ClaireonBlackboardEditToolBase::CreateKeyTypeForName(const FString& TypeName, UObject* Outer, FString& OutError)
{
	static const TMap<FString, UClass*> TypeMap = {
		{ TEXT("Bool"), UBlackboardKeyType_Bool::StaticClass() },
		{ TEXT("Int"), UBlackboardKeyType_Int::StaticClass() },
		{ TEXT("Float"), UBlackboardKeyType_Float::StaticClass() },
		{ TEXT("String"), UBlackboardKeyType_String::StaticClass() },
		{ TEXT("Name"), UBlackboardKeyType_Name::StaticClass() },
		{ TEXT("Vector"), UBlackboardKeyType_Vector::StaticClass() },
		{ TEXT("Rotator"), UBlackboardKeyType_Rotator::StaticClass() },
		{ TEXT("Object"), UBlackboardKeyType_Object::StaticClass() },
		{ TEXT("Class"), UBlackboardKeyType_Class::StaticClass() },
		{ TEXT("Enum"), UBlackboardKeyType_Enum::StaticClass() },
	};

	const UClass* const* FoundClass = TypeMap.Find(TypeName);
	if (!FoundClass)
	{
		OutError = FString::Printf(TEXT("Unknown key type: '%s'. Supported types: Bool, Int, Float, String, Name, Vector, Rotator, Object, Class, Enum"), *TypeName);
		return nullptr;
	}

	return NewObject<UBlackboardKeyType>(Outer, *FoundClass);
}

void ClaireonBlackboardEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonBlackboardEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonBlackboardEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FBlackboardEditToolData*& OutData,
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

FToolResult ClaireonBlackboardEditToolBase::BuildStateResponse(const FString& SessionId, FBlackboardEditToolData* Data)
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
		TSharedPtr<FJsonObject> SuppressJson = MakeShared<FJsonObject>();
		SuppressJson->SetStringField(TEXT("session_id"), SessionId);
		SuppressJson->SetStringField(TEXT("status"), StatusMsg);
		return MakeSuccessResult(SuppressJson, StatusMsg);
	}

	FString BBView;
	BBView += TEXT("=== Session Status ===\n");
	BBView += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	BBView += FString::Printf(TEXT("Asset: %s\n"), *Data->BlackboardData->GetPathName());
	BBView += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);
	BBView += TEXT("\n");
	BBView += ClaireonBehaviorTreeHelpers::FormatBlackboardData(Data->BlackboardData.Get(), false);

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), Data->BlackboardData->GetPathName());
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetStringField(TEXT("last_operation"), Data->LastOperationStatus);
	ResultJson->SetStringField(TEXT("blackboard_view"), BBView);

	const FString Summary = FString::Printf(TEXT("Session %s: %s"),
		*SessionId.Left(8), *Data->LastOperationStatus);

	return MakeSuccessResult(ResultJson, Summary);
}
