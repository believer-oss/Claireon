// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

// TODO(WI-8): register this tool in ClaireonModule.cpp (file owned by no
// fan-out item; integration phase applies this):
//   #include "Tools/ClaireonTool_PIEWaitPoll.h"
//   Tools.Add(MakeShared<ClaireonTool_PIEWaitPoll>());
// Until registered, the same poll is reachable through pie_wait_for's
// wait_id parameter, so no functionality is gated on the registration.

#include "Tools/ClaireonTool_PIEWaitPoll.h"
#include "ClaireonLog.h"
#include "Tools/ClaireonWaitSupport.h"

#include "Dom/JsonObject.h"

FString ClaireonTool_PIEWaitPoll::GetCategory() const { return TEXT("pie"); }
FString ClaireonTool_PIEWaitPoll::GetOperation() const { return TEXT("wait_poll"); }

FString ClaireonTool_PIEWaitPoll::GetDescription() const
{
    return TEXT("Poll a non-blocking PIE wait started by pie_wait_for. Reports {status:'waiting'} until the condition is met or the timeout expires; the terminal state (met or timedOut, with diagnostics) is consumed on read. Stateless / non-session.");
}

TSharedPtr<FJsonObject> ClaireonTool_PIEWaitPoll::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> WaitIdProp = MakeShared<FJsonObject>();
	WaitIdProp->SetStringField(TEXT("type"), TEXT("string"));
	WaitIdProp->SetStringField(TEXT("description"),
		TEXT("The wait_id returned by a pie_wait_for call that answered {status:'waiting'}. A terminal state (met/timedOut) is consumed by the poll that reports it."));
	Properties->SetObjectField(TEXT("wait_id"), WaitIdProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("wait_id")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_PIEWaitPoll::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	UE_LOG(LogClaireon, Display, TEXT("[MCP] editor.pie.waitPoll"));

	FString WaitId;
	if (!Arguments.IsValid() || !Arguments->TryGetStringField(TEXT("wait_id"), WaitId) || WaitId.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: wait_id"));
	}

	return ClaireonWaitSupport::BuildWaitPollResult(FClaireonPIEWaitRegistry::Get(), WaitId);
}
