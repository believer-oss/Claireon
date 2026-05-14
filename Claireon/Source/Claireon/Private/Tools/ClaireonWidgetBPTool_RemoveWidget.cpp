// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_RemoveWidget.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonLog.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_RemoveWidget::GetName() const
{
    return TEXT("claireon.widgetbp_remove_widget");
}

FString ClaireonWidgetBPTool_RemoveWidget::GetDescription() const
{
    return TEXT("Remove a widget from the widget tree by name in the open Widget Blueprint editing session. Requires open session_id from claireon.widgetbp_open. Transactional. The widget and any of its descendants are removed together; bindings targeting the removed widgets become unresolvable until reauthored.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_RemoveWidget::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("widget_name"), TEXT("Name of the widget to remove."), true);
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_RemoveWidget::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("remove_widget"), Params, SessionId, Data, Error))
    {
        return Error;
    }
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
	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Widget")));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();

	if (Tree->RootWidget == Widget)
	{
		Tree->RootWidget = nullptr;
	}
	else
	{
		Tree->RemoveWidget(Widget);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	// Clear focus if the focused widget was removed
	if (Data->FocusedWidget == Widget->GetFName())
	{
		Data->FocusedWidget = Tree->RootWidget ? Tree->RootWidget->GetFName() : NAME_None;
	}

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Removed widget '%s'"), *WidgetName);

	return BuildStateResponse(SessionId, Data);
}

