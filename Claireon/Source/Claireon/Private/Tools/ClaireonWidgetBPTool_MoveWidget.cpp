// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_MoveWidget.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonLog.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_MoveWidget::GetName() const
{
    return TEXT("claireon.widgetbp_move_widget");
}

FString ClaireonWidgetBPTool_MoveWidget::GetDescription() const
{
    return TEXT("Move a widget to a new parent panel (and optional index) within the widget tree.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_MoveWidget::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("widget_name"), TEXT("Name of the widget to move."), true);
    Builder.AddString(TEXT("new_parent_name"), TEXT("Name of the destination panel widget."), true);
    Builder.AddInteger(TEXT("index"), TEXT("Optional insertion index; appends to the end if omitted."));
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_MoveWidget::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("move_widget"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_MoveWidget(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_MoveWidget(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString NewParentName;
	if (!Params->TryGetStringField(TEXT("new_parent_name"), NewParentName))
	{
		return MakeErrorResult(TEXT("Missing required field: new_parent_name"));
	}

	int32 InsertIndex = -1;
	Params->TryGetNumberField(TEXT("index"), InsertIndex);

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

	UWidget* NewParentWidget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*NewParentName));
	if (!NewParentWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("New parent widget '%s' not found"), *NewParentName));
	}

	UPanelWidget* NewParentPanel = Cast<UPanelWidget>(NewParentWidget);
	if (!NewParentPanel)
	{
		return MakeErrorResult(FString::Printf(TEXT("New parent widget '%s' is not a panel widget"), *NewParentName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Move Widget")));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();

	// Remove from old parent if it has one
	if (UPanelWidget* OldParent = Cast<UPanelWidget>(Widget->GetParent()))
	{
		OldParent->RemoveChild(Widget);
	}

	// Add to new parent
	if (InsertIndex >= 0)
	{
		NewParentPanel->InsertChildAt(InsertIndex, Widget);
	}
	else
	{
		NewParentPanel->AddChild(Widget);
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Moved widget '%s' to parent '%s'"), *WidgetName, *NewParentName);

	return BuildStateResponse(SessionId, Data);
}
