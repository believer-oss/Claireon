// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_GetState.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_GetState::GetName() const
{
    return TEXT("claireon.widgetbp_get_state");
}

FString ClaireonWidgetBPTool_GetState::GetDescription() const
{
    return TEXT("Return the current state of an open Widget Blueprint editing session (widget tree, focused widget, modified flag).");
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
    return Operation_GetState(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_GetState(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	return BuildStateResponse(SessionId, Data);
}
