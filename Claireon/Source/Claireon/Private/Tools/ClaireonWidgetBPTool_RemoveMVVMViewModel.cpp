// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_RemoveMVVMViewModel.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewModelContext.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_RemoveMVVMViewModel::GetName() const
{
    return TEXT("claireon.widgetbp_remove_mvvm_viewmodel");
}

FString ClaireonWidgetBPTool_RemoveMVVMViewModel::GetDescription() const
{
    return TEXT("Remove an MVVM ViewModel context from the Widget Blueprint.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_RemoveMVVMViewModel::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("viewmodel_name"), TEXT("Name of the ViewModel context to remove."), true);
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_RemoveMVVMViewModel::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("remove_mvvm_viewmodel"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_RemoveMVVMViewModel(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_RemoveMVVMViewModel(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString ViewModelName;
	if (!Params->TryGetStringField(TEXT("viewmodel_name"), ViewModelName) || ViewModelName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: viewmodel_name"));
	}

	const UMVVMBlueprintView* View = ClaireonWidgetHelpers::GetMVVMBlueprintView(WBP);
	if (!View)
	{
		return MakeErrorResult(TEXT("No MVVM Blueprint View exists on this widget blueprint"));
	}

	const FMVVMBlueprintViewModelContext* Context = View->FindViewModel(FName(*ViewModelName));
	if (!Context)
	{
		return MakeErrorResult(FString::Printf(TEXT("ViewModel '%s' not found"), *ViewModelName));
	}

	FGuid VMId = Context->GetViewModelId();

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove MVVM ViewModel")));

	// Need non-const view for mutation
	UMVVMBlueprintView* MutableView = ClaireonWidgetHelpers::GetOrCreateMVVMBlueprintView(WBP);
	bool bRemoved = MutableView->RemoveViewModel(VMId);
	if (!bRemoved)
	{
		return MakeErrorResult(FString::Printf(TEXT("Failed to remove ViewModel '%s'"), *ViewModelName));
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("removed_viewmodel"), ViewModelName);

	return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Removed MVVM ViewModel '%s'"), *ViewModelName));
}
