// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_ExportWidgets.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "WidgetBlueprintEditorUtils.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_ExportWidgets::GetName() const
{
    return TEXT("claireon.widgetbp_export_widgets");
}

FString ClaireonWidgetBPTool_ExportWidgets::GetDescription() const
{
    return TEXT("Export the named widgets as a serialized text payload (for later import_widgets).");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_ExportWidgets::GetInputSchema() const
{
    // FToolSchemaBuilder has no AddArray; assemble the array property manually.
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    TSharedPtr<FJsonObject> Schema = Builder.Build();

    const TSharedPtr<FJsonObject>* PropsPtr = nullptr;
    if (Schema->TryGetObjectField(TEXT("properties"), PropsPtr) && PropsPtr && (*PropsPtr).IsValid())
    {
        TSharedPtr<FJsonObject> ArrayProp = MakeShared<FJsonObject>();
        ArrayProp->SetStringField(TEXT("type"), TEXT("array"));
        ArrayProp->SetStringField(TEXT("description"), TEXT("Names of widgets in the tree to export."));
        TSharedPtr<FJsonObject> Items = MakeShared<FJsonObject>();
        Items->SetStringField(TEXT("type"), TEXT("string"));
        ArrayProp->SetObjectField(TEXT("items"), Items);
        (*PropsPtr)->SetObjectField(TEXT("widget_names"), ArrayProp);
    }

    const TArray<TSharedPtr<FJsonValue>>* ReqPtr = nullptr;
    if (Schema->TryGetArrayField(TEXT("required"), ReqPtr) && ReqPtr)
    {
        TArray<TSharedPtr<FJsonValue>> Req = *ReqPtr;
        Req.Add(MakeShared<FJsonValueString>(TEXT("widget_names")));
        Schema->SetArrayField(TEXT("required"), Req);
    }
    return Schema;
}

FToolResult ClaireonWidgetBPTool_ExportWidgets::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("export_widgets"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_ExportWidgets(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_ExportWidgets(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* WidgetNamesArray = nullptr;
	if (!Params->TryGetArrayField(TEXT("widget_names"), WidgetNamesArray) || !WidgetNamesArray)
	{
		return MakeErrorResult(TEXT("Missing required field: widget_names (array of strings)"));
	}

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	TArray<UWidget*> WidgetsToExport;
	for (const TSharedPtr<FJsonValue>& NameValue : *WidgetNamesArray)
	{
		FString Name = NameValue->AsString();
		UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*Name));
		if (!Widget)
		{
			return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found"), *Name));
		}
		WidgetsToExport.Add(Widget);
	}

	if (WidgetsToExport.Num() == 0)
	{
		return MakeErrorResult(TEXT("No widgets specified for export"));
	}

	FString ExportedText;
	FWidgetBlueprintEditorUtils::ExportWidgetsToText(WidgetsToExport, ExportedText);

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetStringField(TEXT("widget_text"), ExportedText);
	ResultObj->SetNumberField(TEXT("widget_count"), WidgetsToExport.Num());

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}
