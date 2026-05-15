// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_ListWidgetClasses.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "UObject/UObjectIterator.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_ListWidgetClasses::GetName() const
{
    return TEXT("claireon.widgetbp_list_widget_classes");
}

FString ClaireonWidgetBPTool_ListWidgetClasses::GetDescription() const
{
    return TEXT("List UWidget subclasses the editor can instantiate. Optionally filter by substring and restrict to panel classes.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_ListWidgetClasses::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("filter"), TEXT("Optional case-insensitive substring filter on class name."));
    Builder.AddBoolean(TEXT("panels_only"), TEXT("If true, only return UPanelWidget subclasses."));
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_ListWidgetClasses::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("list_widget_classes"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_ListWidgetClasses(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_ListWidgetClasses(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString Filter;
	Params->TryGetStringField(TEXT("filter"), Filter);

	bool bPanelsOnly = false;
	Params->TryGetBoolField(TEXT("panels_only"), bPanelsOnly);

	TArray<UClass*> FoundClasses;

	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Class = *ClassIt;

		// Must be a non-abstract, non-deprecated UWidget subclass
		if (!Class->IsChildOf(UWidget::StaticClass()))
		{
			continue;
		}
		if (Class->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated))
		{
			continue;
		}

		// panels_only filter
		if (bPanelsOnly && !Class->IsChildOf(UPanelWidget::StaticClass()))
		{
			continue;
		}

		// string filter (case-insensitive match on class name)
		if (!Filter.IsEmpty())
		{
			FString ClassName = Class->GetName();
			if (!ClassName.Contains(Filter, ESearchCase::IgnoreCase))
			{
				continue;
			}
		}

		FoundClasses.Add(Class);
	}

	// Sort alphabetically by class name
	FoundClasses.Sort([](const UClass& A, const UClass& B)
	{
		return A.GetName() < B.GetName();
	});

	TArray<TSharedPtr<FJsonValue>> ClassArray;
	for (UClass* Class : FoundClasses)
	{
		TSharedPtr<FJsonObject> ClassObj = MakeShared<FJsonObject>();
		ClassObj->SetStringField(TEXT("name"), Class->GetName());
		ClassObj->SetBoolField(TEXT("is_panel"), Class->IsChildOf(UPanelWidget::StaticClass()));
		ClassArray.Add(MakeShared<FJsonValueObject>(ClassObj));
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetArrayField(TEXT("classes"), ClassArray);
	ResultObj->SetNumberField(TEXT("count"), ClassArray.Num());

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}
