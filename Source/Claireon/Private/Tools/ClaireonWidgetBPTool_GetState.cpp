// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_GetState.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_GetState::GetOperation() const { return TEXT("get_state"); }

FString ClaireonWidgetBPTool_GetState::GetDescription() const
{
    return TEXT("Return the current state of the open Widget Blueprint editing session: widget tree, focused widget, and modified flag. Requires open session_id from widgetbp_open. Read-only. Use to verify pending changes before widgetbp_save and to discover widget names for downstream operations.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_GetState::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_GetState::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("get_state"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	return BuildStateResponse(SessionId, Data);
}

