// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_AddBinding.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_AddBinding::GetOperation() const { return TEXT("add_binding"); }

FString ClaireonWidgetBPTool_AddBinding::GetDescription() const
{
    return TEXT("Bind a widget property to a Blueprint function (legacy UMG property binding) in the open Widget Blueprint editing session. Requires open session_id from widgetbp_open. Transactional. Common pitfall: prefer widgetbp_add_mvvm_binding for new code; legacy bindings exist for compatibility with older UMG patterns.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_AddBinding::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("widget_name"), TEXT("Name of the widget to bind."), true);
    Builder.AddString(TEXT("property_name"), TEXT("Property on the widget to bind (e.g. Text, Visibility)."), true);
    Builder.AddString(TEXT("function_name"), TEXT("Blueprint function that provides the bound value."), true);
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_AddBinding::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("add_binding"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	FString WidgetNameStr;
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_name"));
	}

	FString PropertyNameStr;
	if (!Params->TryGetStringField(TEXT("property_name"), PropertyNameStr))
	{
		return MakeErrorResult(TEXT("Missing required field: property_name"));
	}

	FString FunctionNameStr;
	if (!Params->TryGetStringField(TEXT("function_name"), FunctionNameStr))
	{
		return MakeErrorResult(TEXT("Missing required field: function_name"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	// Validate the widget exists
	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetNameStr));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *WidgetNameStr));
	}

	// Build the binding
	FDelegateEditorBinding Binding;
	Binding.ObjectName = WidgetNameStr;
	Binding.PropertyName = FName(*PropertyNameStr);
	Binding.FunctionName = FName(*FunctionNameStr);
	Binding.Kind = EBindingKind::Function;

	// Remove any pre-existing binding for the same widget+property (only one allowed)
	WBP->Bindings.RemoveAll([&](const FDelegateEditorBinding& Existing)
	{
		return Existing.ObjectName == Binding.ObjectName && Existing.PropertyName == Binding.PropertyName;
	});

	WBP->Bindings.Add(Binding);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_name"), WidgetNameStr);
	ResultObj->SetStringField(TEXT("property_name"), PropertyNameStr);
	ResultObj->SetStringField(TEXT("function_name"), FunctionNameStr);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}

