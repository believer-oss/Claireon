// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonEQSEditToolBase.h"
#include "Tools/ClaireonBehaviorTreeHelpers.h"
#include "ClaireonNameResolver.h"
#include "ClaireonSessionManager.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "EnvironmentQuery/EnvQueryOption.h"
#include "EnvironmentQuery/EnvQueryGenerator.h"
#include "EnvironmentQuery/EnvQueryTest.h"

using FToolResult = IClaireonTool::FToolResult;

// ============================================================================
// Static members
// ============================================================================

const TCHAR* ClaireonEQSEditToolBase::EQSSessionToolName = TEXT("claireon.eqs_edit");
TMap<FString, FEQSEditToolData> ClaireonEQSEditToolBase::ToolData;
bool ClaireonEQSEditToolBase::bDelegateRegistered = false;

void ClaireonEQSEditToolBase::HandleSessionClosed(const FMCPSessionClosedInfo& Info)
{
	if (Info.ToolName == EQSSessionToolName)
	{
		ToolData.Remove(Info.SessionId);
	}
}

void ClaireonEQSEditToolBase::EnsureDelegateRegistered()
{
	if (!bDelegateRegistered)
	{
		FClaireonSessionManager::Get().OnSessionClosed().AddStatic(&ClaireonEQSEditToolBase::HandleSessionClosed);
		bDelegateRegistered = true;
	}
}

// ============================================================================
// EQS-specific helpers (moved from monolithic ClaireonTool_EQSEdit.cpp)
// ============================================================================

TArray<UEnvQueryOption*>& ClaireonEQSEditToolBase::GetOptionsMutable(UEnvQuery* Query)
{
	// Access the Options UPROPERTY via reflection for safe mutable access
	static FArrayProperty* OptionsProp = nullptr;
	if (!OptionsProp)
	{
		OptionsProp = CastField<FArrayProperty>(FindFProperty<FProperty>(UEnvQuery::StaticClass(), TEXT("Options")));
	}
	check(OptionsProp);
	return *OptionsProp->ContainerPtrToValuePtr<TArray<UEnvQueryOption*>>(Query);
}

UClass* ClaireonEQSEditToolBase::ResolveEQSClass(const FString& ClassName, UClass* BaseClass, const FString& BasePrefix, FString& OutError)
{
	// Try the core resolver first
	ClaireonNameResolver::FNameResolveResult NameResult;
	UClass* FoundClass = ClaireonNameResolver::ResolveClassName(ClassName, BaseClass, NameResult);
	if (FoundClass)
	{
		return FoundClass;
	}

	// Try with domain-specific prefix (e.g., "EnvQueryGenerator_" + "SimpleGrid")
	FoundClass = ClaireonNameResolver::ResolveClassName(BasePrefix + ClassName, BaseClass, NameResult);
	if (FoundClass)
	{
		return FoundClass;
	}

	OutError = FString::Printf(TEXT("Could not resolve EQS class: %s (expected subclass of %s)"), *ClassName, *BaseClass->GetName());
	return nullptr;
}

bool ClaireonEQSEditToolBase::SetEQSNodeProperty(UObject* Node, const FString& PropertyName, const FString& PropertyValue, FString& OutError)
{
	if (!Node)
	{
		OutError = TEXT("Node is null");
		return false;
	}

	FProperty* Property = FindFProperty<FProperty>(Node->GetClass(), *PropertyName);
	if (!Property)
	{
		OutError = FString::Printf(TEXT("Property '%s' not found on %s"), *PropertyName, *Node->GetClass()->GetName());
		return false;
	}

	void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Node);
	const TCHAR* Result = Property->ImportText_Direct(*PropertyValue, ValuePtr, Node, PPF_None);
	if (!Result)
	{
		OutError = FString::Printf(TEXT("Failed to set property '%s' to '%s' on %s"), *PropertyName, *PropertyValue, *Node->GetClass()->GetName());
		return false;
	}

	return true;
}

// ============================================================================
// Session helpers
// ============================================================================

bool ClaireonEQSEditToolBase::RequireSession(
	const TSharedPtr<FJsonObject>& Arguments,
	FString& OutSessionId,
	FEQSEditToolData*& OutData,
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

FToolResult ClaireonEQSEditToolBase::BuildStateResponse(const FString& SessionId, FEQSEditToolData* Data)
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

	FString Output;
	Output += TEXT("=== Session Status ===\n");
	Output += FString::Printf(TEXT("Session: %s\n"), *SessionId);
	Output += FString::Printf(TEXT("Asset: %s\n"), *Data->Query->GetPathName());
	Output += FString::Printf(TEXT("Last Operation: %s\n"), *Data->LastOperationStatus);
	Output += TEXT("\n");
	Output += ClaireonBehaviorTreeHelpers::FormatEQSStructure(Data->Query.Get(), false);

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("asset_path"), Data->Query->GetPathName());
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetStringField(TEXT("last_operation"), Data->LastOperationStatus);
	ResultJson->SetStringField(TEXT("eqs_view"), Output);

	const FString Summary = FString::Printf(TEXT("Session %s: %s"),
		*SessionId.Left(8), *Data->LastOperationStatus);

	return MakeSuccessResult(ResultJson, Summary);
}
