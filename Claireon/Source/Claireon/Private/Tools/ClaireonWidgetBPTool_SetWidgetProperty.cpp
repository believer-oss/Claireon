// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_SetWidgetProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_SetWidgetProperty::GetName() const
{
    return TEXT("claireon.widgetbp_set_widget_property");
}

FString ClaireonWidgetBPTool_SetWidgetProperty::GetDescription() const
{
    return TEXT("Set a property on a widget by name in the open Widget Blueprint editing session (top-level widget property, not slot property). Requires open session_id from claireon.widgetbp_open. Transactional. Common pitfall: slot properties (Anchors, Offsets, Size) live on the slot, not the widget; use claireon.widgetbp_set_slot_property for those.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_SetWidgetProperty::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("widget_name"), TEXT("Name of the widget to modify."), true);
    Builder.AddString(TEXT("property_name"), TEXT("Property name on the widget."), true);
    Builder.AddString(TEXT("value"), TEXT("String representation of the value (imported via the property's text format)."), true);
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_SetWidgetProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("set_widget_property"), Params, SessionId, Data, Error))
    {
        return Error;
    }
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Widget Property")));
	Widget->Modify();

	FString WriteError;
	if (!ClaireonWidgetHelpers::WriteWidgetProperty(Widget, PropertyName, Value, WriteError))
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to set property '%s' on widget '%s': %s"), *PropertyName, *WidgetName, *WriteError));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	return BuildStateResponse(SessionId, Data);
}

