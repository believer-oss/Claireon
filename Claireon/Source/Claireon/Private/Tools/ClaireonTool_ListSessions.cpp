// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_ListSessions.h"
#include "ClaireonSessionManager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

FString ClaireonTool_ListSessions::GetCategory() const { return TEXT("session"); }
FString ClaireonTool_ListSessions::GetOperation() const { return TEXT("list"); }

FString ClaireonTool_ListSessions::GetDescription() const
{
	return TEXT("List all active MCP editing sessions. Optionally filter by tool name. "
				"Returns session IDs, tool names, asset paths, and timing information.");
}

TSharedPtr<FJsonObject> ClaireonTool_ListSessions::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	// tool_name (optional filter)
	TSharedPtr<FJsonObject> ToolNameProp = MakeShared<FJsonObject>();
	ToolNameProp->SetStringField(TEXT("type"), TEXT("string"));
	ToolNameProp->SetStringField(TEXT("description"),
		TEXT("Optional tool name filter (e.g. 'niagara_edit'). If omitted, returns all sessions."));
	Properties->SetObjectField(TEXT("tool_name"), ToolNameProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_ListSessions::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	const FString ToolNameFilter = Arguments->GetStringField(TEXT("tool_name"));

	const TArray<FMCPSession> Sessions = FClaireonSessionManager::Get().ListSessions(ToolNameFilter);

	TArray<TSharedPtr<FJsonValue>> SessionArray;
	SessionArray.Reserve(Sessions.Num());

	for (const FMCPSession& Session : Sessions)
	{
		TSharedPtr<FJsonObject> SessionObj = MakeShared<FJsonObject>();
		SessionObj->SetStringField(TEXT("session_id"), Session.SessionId);
		SessionObj->SetStringField(TEXT("tool_name"), Session.ToolName);
		SessionObj->SetStringField(TEXT("asset_path"), Session.AssetPath);
		SessionObj->SetStringField(TEXT("created_time"), Session.CreatedTime.ToIso8601());
		SessionObj->SetStringField(TEXT("last_access_time"), Session.LastAccessTime.ToIso8601());
		SessionObj->SetNumberField(TEXT("timeout_minutes"), Session.TimeoutMinutes);
		SessionArray.Add(MakeShared<FJsonValueObject>(SessionObj));
	}

	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetArrayField(TEXT("sessions"), SessionArray);
	Data->SetNumberField(TEXT("count"), Sessions.Num());

	const FString Summary = FString::Printf(TEXT("Found %d active session(s)%s"),
		Sessions.Num(),
		ToolNameFilter.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" for tool '%s'"), *ToolNameFilter));

	return MakeSuccessResult(Data, Summary);
}