// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TraceClose.h"
#include "ClaireonLog.h"
#include "ClaireonTraceSession.h"

FString ClaireonTool_TraceClose::GetCategory() const { return TEXT("trace"); }
FString ClaireonTool_TraceClose::GetOperation() const { return TEXT("close"); }

FString ClaireonTool_TraceClose::GetDescription() const
{
	return TEXT("Close a trace analysis session and free memory");
}

TSharedPtr<FJsonObject> ClaireonTool_TraceClose::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> SessionIdProp = MakeShared<FJsonObject>();
	SessionIdProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionIdProp->SetStringField(TEXT("description"), TEXT("The session ID returned by editor.trace.open"));
	Properties->SetObjectField(TEXT("sessionId"), SessionIdProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("sessionId")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_TraceClose::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	if (!Arguments->TryGetStringField(TEXT("sessionId"), SessionId) || SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required field: sessionId"));
	}

	const bool bClosed = FClaireonTraceSessionManager::Get().CloseSession(SessionId);

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetStringField(TEXT("session_id"), SessionId);
	Data->SetBoolField(TEXT("closed"), bClosed);

	const FString Summary = FString::Printf(TEXT("Closed trace session %s"), *SessionId);
	return MakeSuccessResult(Data, Summary);
}
