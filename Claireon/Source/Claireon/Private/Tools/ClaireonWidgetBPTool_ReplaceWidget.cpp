// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_ReplaceWidget.h"
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

FString ClaireonWidgetBPTool_ReplaceWidget::GetOperation() const { return TEXT("replace_widget"); }

FString ClaireonWidgetBPTool_ReplaceWidget::GetDescription() const
{
    return TEXT("Replace a widget with a new class in the open Widget Blueprint editing session, optionally preserving its children. Requires open session_id from widgetbp_open. Transactional. Common pitfall: properties not present on the new class are dropped; verify with widgetbp_get_widget_details after.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_ReplaceWidget::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("widget_name"), TEXT("Name of the widget to replace."), true);
    Builder.AddString(TEXT("new_widget_class"), TEXT("New widget class name or path."), true);
    Builder.AddBoolean(TEXT("preserve_children"), TEXT("If true (default), reparent existing children to the replacement widget when it's a panel."));
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_ReplaceWidget::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("replace_widget"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString NewWidgetClassStr;
	if (!Params->TryGetStringField(TEXT("new_widget_class"), NewWidgetClassStr))
	{
		return MakeErrorResult(TEXT("Missing required field: new_widget_class"));
	}

	bool bPreserveChildren = true;
	Params->TryGetBoolField(TEXT("preserve_children"), bPreserveChildren);

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	UWidget* OldWidget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*WidgetName));
	if (!OldWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	FString ClassError;
	UClass* NewClass = ClaireonWidgetHelpers::ResolveWidgetClass(NewWidgetClassStr, ClassError);
	if (!NewClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not resolve new widget class '%s': %s"), *NewWidgetClassStr, *ClassError));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Replace Widget")));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();

	// Collect children from old widget before replacing (if it's a panel)
	TArray<UWidget*> OldChildren;
	UPanelWidget* OldPanel = Cast<UPanelWidget>(OldWidget);
	if (bPreserveChildren && OldPanel)
	{
		for (int32 i = 0; i < OldPanel->GetChildrenCount(); ++i)
		{
			OldChildren.Add(OldPanel->GetChildAt(i));
		}
	}

	// Create new widget
	UWidget* NewWidget = ClaireonWidgetHelpers::CreateWidget(Tree, NewClass, NAME_None);
	if (!NewWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create new widget of class '%s'"), *NewWidgetClassStr));
	}

	// Note old parent before replacing
	UPanelWidget* ParentPanel = Cast<UPanelWidget>(OldWidget->GetParent());

	if (Tree->RootWidget == OldWidget)
	{
		// Replace root
		Tree->RootWidget = NewWidget;
	}
	else if (ParentPanel)
	{
		// Use ReplaceChild to maintain slot position
		if (!ParentPanel->ReplaceChild(OldWidget, NewWidget))
		{
			// Fallback: remove old and add new
			ParentPanel->RemoveChild(OldWidget);
			ParentPanel->AddChild(NewWidget);
		}
	}

	// Reparent children to the new widget if it's a panel
	UPanelWidget* NewPanel = Cast<UPanelWidget>(NewWidget);
	if (OldChildren.Num() > 0 && NewPanel)
	{
		for (UWidget* Child : OldChildren)
		{
			if (OldPanel)
			{
				OldPanel->RemoveChild(Child);
			}
			NewPanel->AddChild(Child);
		}
		UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Reparented %d children to replacement widget"), OldChildren.Num());
	}
	else if (OldChildren.Num() > 0 && !NewPanel)
	{
		UE_LOG(LogClaireon, Warning, TEXT("[EditWidgetBP] Replacement widget '%s' is not a panel -- %d children were lost"), *NewWidgetClassStr, OldChildren.Num());
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	// Update focus if needed
	if (Data->FocusedWidget == OldWidget->GetFName())
	{
		Data->FocusedWidget = NewWidget->GetFName();
	}

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Replaced widget '%s' with class '%s' -> '%s'"), *WidgetName, *NewWidgetClassStr, *NewWidget->GetName());

	return BuildStateResponse(SessionId, Data);
}

