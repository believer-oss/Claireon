// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_RemoveBinding.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "WidgetBlueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_RemoveBinding::GetName() const
{
    return TEXT("claireon.widgetbp_remove_binding");
}

FString ClaireonWidgetBPTool_RemoveBinding::GetDescription() const
{
    return TEXT("Remove a legacy UMG property binding from a widget.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_RemoveBinding::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("widget_name"), TEXT("Widget whose binding should be removed."), true);
    Builder.AddString(TEXT("property_name"), TEXT("Property whose binding should be removed."), true);
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_RemoveBinding::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("remove_binding"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_RemoveBinding(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_RemoveBinding(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
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

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	int32 RemovedCount = WBP->Bindings.RemoveAll([&](const FDelegateEditorBinding& Binding)
	{
		return Binding.ObjectName == WidgetNameStr && Binding.PropertyName == FName(*PropertyNameStr);
	});

	if (RemovedCount > 0)
	{
		FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
		Data->bModified = true;
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetNumberField(TEXT("removed_count"), RemovedCount);

	FString ResultString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}
