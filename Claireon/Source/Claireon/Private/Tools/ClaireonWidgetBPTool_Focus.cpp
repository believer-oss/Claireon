// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_Focus.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_Focus::GetName() const
{
    return TEXT("claireon.widgetbp_focus");
}

FString ClaireonWidgetBPTool_Focus::GetDescription() const
{
    return TEXT("Set the focused widget within an open Widget Blueprint editing session.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_Focus::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("widget_name"), TEXT("Widget name (in the widget tree) to focus."), true);
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_Focus::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("focus"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_Focus(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_Focus(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	UWidget* FoundWidget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetName));
	if (!FoundWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found in widget tree"), *WidgetName));
	}

	Data->FocusedWidget = FName(*WidgetName);

	return BuildStateResponse(SessionId, Data);
}
