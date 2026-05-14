// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_GetWidgetDetails.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_GetWidgetDetails::GetName() const
{
    return TEXT("claireon.widgetbp_get_widget_details");
}

FString ClaireonWidgetBPTool_GetWidgetDetails::GetDescription() const
{
    return TEXT("Return detailed information about a single widget in the open Widget Blueprint editing session: class, parent, children, editable properties, slot properties. Requires open session_id from claireon.widgetbp_open. Read-only. Pair with claireon.widgetbp_get_state to discover names, then drill in here for property detail.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_GetWidgetDetails::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("widget_name"), TEXT("Name of the widget to inspect."), true);
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_GetWidgetDetails::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("get_widget_details"), Params, SessionId, Data, Error))
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

	TSharedPtr<FJsonObject> Details = MakeShared<FJsonObject>();
	Details->SetStringField(TEXT("name"), Widget->GetName());
	Details->SetStringField(TEXT("class"), Widget->GetClass()->GetPathName());

	// Parent info
	if (UWidget* Parent = Widget->GetParent())
	{
		Details->SetStringField(TEXT("parent_name"), Parent->GetName());
	}
	else
	{
		Details->SetStringField(TEXT("parent_name"), TEXT(""));
	}

	// Panel info
	UPanelWidget* AsPanel = Cast<UPanelWidget>(Widget);
	Details->SetBoolField(TEXT("is_panel"), AsPanel != nullptr);

	if (AsPanel)
	{
		Details->SetNumberField(TEXT("child_count"), AsPanel->GetChildrenCount());

		TArray<TSharedPtr<FJsonValue>> ChildNames;
		for (int32 i = 0; i < AsPanel->GetChildrenCount(); ++i)
		{
			if (UWidget* Child = AsPanel->GetChildAt(i))
			{
				ChildNames.Add(MakeShared<FJsonValueString>(Child->GetName()));
			}
		}
		Details->SetArrayField(TEXT("children"), ChildNames);
	}

	// Slot info
	if (UPanelSlot* Slot = Widget->Slot)
	{
		Details->SetStringField(TEXT("slot_type"), Slot->GetClass()->GetName());

		TSharedPtr<FJsonObject> SlotProps = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> PropIt(Slot->GetClass()); PropIt; ++PropIt)
		{
			FProperty* Prop = *PropIt;
			if (!Prop->HasAnyPropertyFlags(CPF_Edit))
			{
				continue;
			}

			bool bSuccess = false;
			FString PropValue = ClaireonWidgetHelpers::ReadSlotProperty(Slot, Prop->GetName(), bSuccess);
			if (bSuccess)
			{
				SlotProps->SetStringField(Prop->GetName(), PropValue);
			}
		}
		Details->SetObjectField(TEXT("slot_properties"), SlotProps);
	}

	// All editable widget properties
	TSharedPtr<FJsonObject> WidgetProps = MakeShared<FJsonObject>();
	for (TFieldIterator<FProperty> PropIt(Widget->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Prop = *PropIt;
		if (!Prop->HasAnyPropertyFlags(CPF_Edit))
		{
			continue;
		}

		bool bSuccess = false;
		FString PropValue = ClaireonWidgetHelpers::ReadWidgetProperty(Widget, Prop->GetName(), bSuccess);
		if (bSuccess)
		{
			WidgetProps->SetStringField(Prop->GetName(), PropValue);
		}
	}
	Details->SetObjectField(TEXT("properties"), WidgetProps);

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(Details.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

