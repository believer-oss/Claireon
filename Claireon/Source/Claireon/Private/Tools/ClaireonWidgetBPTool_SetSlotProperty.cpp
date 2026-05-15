// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_SetSlotProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_SetSlotProperty::GetName() const
{
    return TEXT("claireon.widgetbp_set_slot_property");
}

FString ClaireonWidgetBPTool_SetSlotProperty::GetDescription() const
{
    return TEXT("Set a slot property on a widget (e.g. Canvas Panel Slot.Anchors, Size, Offsets).");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_SetSlotProperty::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("widget_name"), TEXT("Name of the widget whose slot should be modified."), true);
    Builder.AddString(TEXT("property_name"), TEXT("Slot property name (e.g. 'Anchors', 'Size', 'Padding')."), true);
    Builder.AddString(TEXT("value"), TEXT("String representation of the value (will be imported via the property's text format)."), true);
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_SetSlotProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("set_slot_property"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_SetSlotProperty(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_SetSlotProperty(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetName;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetName))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString PropertyName;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyName))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}

	FString Value;
	if (!Params->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("Missing required field: value"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetName));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetName));
	}

	UPanelSlot* Slot = Widget->Slot;
	if (!Slot)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' has no slot (it may be the root widget)"), *WidgetName));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Slot Property")));
	Slot->Modify();

	FString WriteError;
	if (!ClaireonWidgetHelpers::WriteSlotProperty(Slot, PropertyName, Value, WriteError))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to set slot property '%s' on widget '%s': %s"), *PropertyName, *WidgetName, *WriteError));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	return BuildStateResponse(SessionId, Data);
}
