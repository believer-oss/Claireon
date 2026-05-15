// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_Close.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonSessionManager.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_Close::GetName() const
{
    return TEXT("claireon.widgetbp_close");
}

FString ClaireonWidgetBPTool_Close::GetDescription() const
{
    return TEXT("Close an open Widget Blueprint editing session, releasing the lock.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_Close::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_Close::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("close"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_Close(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_Close(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	bool bWasModified = Data->bModified;

	// Remove tool data before closing session (delegate will also try, but be explicit)
	ToolData.Remove(SessionId);
	FClaireonSessionManager::Get().CloseSession(SessionId);

	FString ResultMsg = TEXT("Session closed successfully.");
	if (bWasModified)
	{
		ResultMsg += TEXT(" Warning: Widget Blueprint had unsaved modifications.");
	}

	TSharedPtr<FJsonObject> ResultJson = MakeShared<FJsonObject>();
	ResultJson->SetStringField(TEXT("session_id"), SessionId);
	ResultJson->SetBoolField(TEXT("success"), true);
	ResultJson->SetStringField(TEXT("message"), ResultMsg);
	if (bWasModified)
	{
		ResultJson->SetBoolField(TEXT("unsaved_changes"), true);
	}

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultJson.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}
