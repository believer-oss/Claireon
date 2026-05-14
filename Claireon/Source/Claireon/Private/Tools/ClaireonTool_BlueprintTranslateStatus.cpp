// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_BlueprintTranslateStatus.h"

#include "ClaireonBPTranslateSession.h"

FString ClaireonTool_BlueprintTranslateStatus::GetCategory() const { return TEXT("blueprint"); }
FString ClaireonTool_BlueprintTranslateStatus::GetOperation() const { return TEXT("translate_status"); }

FString ClaireonTool_BlueprintTranslateStatus::GetDescription() const
{
	return TEXT("Query the completion status of a BP-to-C++ translation session. Returns per-blueprint "
		"node counts (total, implemented, skipped, pending) and overall completion percentage.");
}

TSharedPtr<FJsonObject> ClaireonTool_BlueprintTranslateStatus::GetInputSchema() const
{
	TSharedPtr<FJsonObject> Schema = MakeShared<FJsonObject>();
	Schema->SetStringField(TEXT("type"), TEXT("object"));

	TSharedPtr<FJsonObject> Properties = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> SessionProp = MakeShared<FJsonObject>();
	SessionProp->SetStringField(TEXT("type"), TEXT("string"));
	SessionProp->SetStringField(TEXT("description"), TEXT("Session ID returned by the scaffold tool."));
	Properties->SetObjectField(TEXT("session_id"), SessionProp);

	TSharedPtr<FJsonObject> FileProp = MakeShared<FJsonObject>();
	FileProp->SetStringField(TEXT("type"), TEXT("string"));
	FileProp->SetStringField(TEXT("description"), TEXT("Direct path to the session JSON file."));
	Properties->SetObjectField(TEXT("session_file"), FileProp);

	Schema->SetObjectField(TEXT("properties"), Properties);

	TArray<TSharedPtr<FJsonValue>> Required;
	Required.Add(MakeShared<FJsonValueString>(TEXT("session_id")));
	Schema->SetArrayField(TEXT("required"), Required);

	return Schema;
}

IClaireonTool::FToolResult ClaireonTool_BlueprintTranslateStatus::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId = Arguments->GetStringField(TEXT("session_id"));
	FString SessionFile = Arguments->GetStringField(TEXT("session_file"));

	if (SessionId.IsEmpty())
	{
		return MakeErrorResult(TEXT("'session_id' is required."));
	}

	FString ResolveError;
	TSharedPtr<FClaireonBPTranslateSession> Session = FClaireonBPTranslateSession::ResolveSession(
		SessionId, SessionFile, ResolveError);
	if (!Session.IsValid())
	{
		return MakeErrorResult(ResolveError);
	}

	TSharedPtr<FJsonObject> Stats = Session->GetCompletionStats();

	int32 TotalNodes = 0;
	int32 ImplementedNodes = 0;
	int32 PendingNodes = 0;
	for (const auto& BPPair : Session->Blueprints)
	{
		TotalNodes += BPPair.Value.TotalNodes;
		ImplementedNodes += BPPair.Value.ImplementedNodes;
		PendingNodes += BPPair.Value.TotalNodes - BPPair.Value.ImplementedNodes - BPPair.Value.SkippedNodes;
	}

	FString Summary = FString::Printf(TEXT("Session %s: %d/%d nodes implemented, %d pending"),
		*SessionId, ImplementedNodes, TotalNodes, PendingNodes);

	return MakeSuccessResult(Stats, Summary);
}
