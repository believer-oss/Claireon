// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_AddWidget.h"
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

FString ClaireonWidgetBPTool_AddWidget::GetOperation() const { return TEXT("add_widget"); }

FString ClaireonWidgetBPTool_AddWidget::GetDescription() const
{
    return TEXT("Add a new widget of a given class into the widget tree in the open Widget Blueprint editing session. Requires open session_id from widgetbp_open. Transactional. Provide widget_class (required) and optional parent_name, widget_name, index. Returns the new widget's name for downstream operations.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_AddWidget::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("widget_class"), TEXT("Widget class name or path (e.g. 'Button', 'TextBlock', '/Script/UMG.Border')."), true);
    Builder.AddString(TEXT("parent_name"), TEXT("Optional parent panel widget name; defaults to root panel if unspecified."));
    Builder.AddString(TEXT("widget_name"), TEXT("Optional explicit name for the new widget."));
    Builder.AddInteger(TEXT("index"), TEXT("Optional insertion index into the parent panel's children."));
    Builder.AddObject(TEXT("slot_properties"), TEXT("Optional map of slot property name -> string value for the new widget's slot."));
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_AddWidget::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("add_widget"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	// Extract required widget_class
	FString WidgetClassStr;
	if (!Params->TryGetStringField(TEXT("widget_class"), WidgetClassStr))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_class"));
	}

	// Extract optional fields
	FString ParentName;
	Params->TryGetStringField(TEXT("parent_name"), ParentName);

	FString WidgetName;
	Params->TryGetStringField(TEXT("widget_name"), WidgetName);

	int32 InsertIndex = -1;
	Params->TryGetNumberField(TEXT("index"), InsertIndex);

	const TSharedPtr<FJsonObject>* SlotPropertiesPtr = nullptr;
	TSharedPtr<FJsonObject> SlotProperties;
	if (Params->TryGetObjectField(TEXT("slot_properties"), SlotPropertiesPtr) && SlotPropertiesPtr)
	{
		SlotProperties = *SlotPropertiesPtr;
	}

	// Get WBP and WidgetTree from session
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}
	UWidgetTree* Tree = WBP->WidgetTree;

	// Resolve class
	FString ClassError;
	UClass* ResolvedClass = ClaireonWidgetHelpers::ResolveWidgetClass(WidgetClassStr, ClassError);
	if (!ResolvedClass)
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not resolve widget class '%s': %s"), *WidgetClassStr, *ClassError));
	}

	// Determine parent panel
	UPanelWidget* ParentPanel = nullptr;
	if (!ParentName.IsEmpty())
	{
		UWidget* ParentWidget = ClaireonWidgetHelpers::FindWidgetByName(Tree, FName(*ParentName));
		if (!ParentWidget)
		{
			return MakeErrorResult(FString::Printf(TEXT("Parent widget '%s' not found"), *ParentName));
		}
		ParentPanel = Cast<UPanelWidget>(ParentWidget);
		if (!ParentPanel)
		{
			return MakeErrorResult(FString::Printf(TEXT("Parent widget '%s' is not a panel widget"), *ParentName));
		}
	}
	else if (Tree->RootWidget)
	{
		// If no parent specified and root is a panel, use root as default parent
		ParentPanel = Cast<UPanelWidget>(Tree->RootWidget);
	}
	// If no root exists at all, we will set the new widget as root

	// Transaction
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Widget")));
	Tree->SetFlags(RF_Transactional);
	Tree->Modify();

	// Determine widget name
	FName FinalWidgetName = WidgetName.IsEmpty() ? NAME_None : FName(*WidgetName);

	// Create the widget
	UWidget* NewWidget = ClaireonWidgetHelpers::CreateWidget(Tree, ResolvedClass, FinalWidgetName);
	if (!NewWidget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to create widget of class '%s'"), *WidgetClassStr));
	}

	// Set as root if no root exists
	if (!Tree->RootWidget)
	{
		Tree->RootWidget = NewWidget;
	}
	else if (ParentPanel)
	{
		// Add to parent panel at specified index or end
		if (InsertIndex >= 0)
		{
			UPanelSlot* Slot = ParentPanel->InsertChildAt(InsertIndex, NewWidget);
			if (Slot && SlotProperties.IsValid())
			{
				for (auto& Pair : SlotProperties->Values)
				{
					FString SlotError;
					ClaireonWidgetHelpers::WriteSlotProperty(Slot, Pair.Key, Pair.Value->AsString(), SlotError);
				}
			}
		}
		else
		{
			ClaireonWidgetHelpers::AddChildToPanel(ParentPanel, NewWidget, SlotProperties);
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;
	Data->FocusedWidget = NewWidget->GetFName();

	UE_LOG(LogClaireon, Log, TEXT("[EditWidgetBP] Added widget '%s' (class: %s)"), *NewWidget->GetName(), *ResolvedClass->GetName());

	return BuildStateResponse(SessionId, Data);
}

