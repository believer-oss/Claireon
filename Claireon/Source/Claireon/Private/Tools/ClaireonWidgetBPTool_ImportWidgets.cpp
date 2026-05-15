// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_ImportWidgets.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Components/PanelWidget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "WidgetBlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_ImportWidgets::GetName() const
{
    return TEXT("claireon.widgetbp_import_widgets");
}

FString ClaireonWidgetBPTool_ImportWidgets::GetDescription() const
{
    return TEXT("Import widgets from the UE clipboard/text format (produced by export_widgets).");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_ImportWidgets::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("widget_text"), TEXT("Clipboard/text representation of widgets to import."), true);
    Builder.AddString(TEXT("parent_name"), TEXT("Optional parent panel to adopt imported root-level widgets."));
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_ImportWidgets::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("import_widgets"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_ImportWidgets(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_ImportWidgets(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetText;
	if (!Params->TryGetStringField(TEXT("widget_text"), WidgetText))
	{
		return MakeErrorResult(TEXT("Missing required field: widget_text"));
	}

	FString ParentName;
	Params->TryGetStringField(TEXT("parent_name"), ParentName);

	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP || !WBP->WidgetTree)
	{
		return MakeErrorResult(TEXT("Widget Blueprint or WidgetTree is no longer valid"));
	}

	TSet<UWidget*> ImportedWidgets;
	TMap<FName, UWidgetSlotPair*> PastedExtraSlotData;

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Import Widgets")));
	WBP->WidgetTree->SetFlags(RF_Transactional);
	WBP->WidgetTree->Modify();

	FWidgetBlueprintEditorUtils::ImportWidgetsFromText(WBP, WidgetText, ImportedWidgets, PastedExtraSlotData);

	if (ImportedWidgets.Num() == 0)
	{
		return MakeErrorResult(TEXT("No widgets were imported from the provided text"));
	}

	// If a parent is specified and imported widgets have no parent, add them to the named parent
	if (!ParentName.IsEmpty())
	{
		UWidget* ParentWidget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*ParentName));
		UPanelWidget* ParentPanel = Cast<UPanelWidget>(ParentWidget);
		if (ParentPanel)
		{
			for (UWidget* Imported : ImportedWidgets)
			{
				if (Imported && !Imported->GetParent())
				{
					ParentPanel->AddChild(Imported);
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	// Collect imported widget names
	TArray<TSharedPtr<FJsonValue>> ImportedNames;
	for (UWidget* Imported : ImportedWidgets)
	{
		if (Imported)
		{
			ImportedNames.Add(MakeShared<FJsonValueString>(Imported->GetName()));
		}
	}

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetBoolField(TEXT("success"), true);
	ResultObj->SetArrayField(TEXT("imported_widgets"), ImportedNames);
	ResultObj->SetNumberField(TEXT("count"), ImportedNames.Num());

	FString ResultString;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&ResultString);
	FJsonSerializer::Serialize(ResultObj.ToSharedRef(), Writer);

	return MakeErrorResult(ResultString);
}
