// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_RenameWidget.h"
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

FString ClaireonWidgetBPTool_RenameWidget::GetName() const
{
    return TEXT("claireon.widgetbp_rename_widget");
}

FString ClaireonWidgetBPTool_RenameWidget::GetDescription() const
{
    return TEXT("Rename a widget within the widget tree in the open Widget Blueprint editing session. Requires open session_id from claireon.widgetbp_open. Transactional. The new name must be unique across the widget tree. Bindings and references to the old name are auto-fixed where possible during the rename.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_RenameWidget::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("widget_name"), TEXT("Current widget name."), true);
    Builder.AddString(TEXT("new_name"), TEXT("New widget name (must not collide with an existing widget)."), true);
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_RenameWidget::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("rename_widget"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString NewName;
	if (!Params->TryGetStringField(TEXT("new_name"), NewName))
	{
		return MakeErrorResult(TEXT("Missing required field: new_name"));
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

	// Verify new name doesn't conflict with an existing widget
	if (ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*NewName)) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("A widget named '%s' already exists in the tree"), *NewName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Rename Widget")));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();
	Widget->Modify();

	FName OldFName = Widget->GetFName();

	// Rename the UObject
	Widget->Rename(*NewName, Widget->GetOuter());

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	// Update focus if the focused widget was renamed
	if (Data->FocusedWidget == OldFName)
	{
		Data->FocusedWidget = Widget->GetFName();
	}

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Renamed widget '%s' to '%s'"), *WidgetName, *Widget->GetName());

	return BuildStateResponse(SessionId, Data);
}

