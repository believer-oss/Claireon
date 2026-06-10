// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_RemoveMVVMBinding.h"
#include "Tools/FToolSchemaBuilder.h"
#include "ClaireonWidgetHelpers.h"
#include "Dom/JsonObject.h"
#include "WidgetBlueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_RemoveMVVMBinding::GetOperation() const { return TEXT("remove_mvvm_binding"); }

FString ClaireonWidgetBPTool_RemoveMVVMBinding::GetDescription() const
{
    return TEXT("Remove an MVVM binding by id from the Widget Blueprint in the open editing session. Requires open session_id from widgetbp_open. Transactional. Common pitfall: the binding_id must match the id returned by widgetbp_list_mvvm_bindings; deleting a binding does not affect the viewmodel context itself.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_RemoveMVVMBinding::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("binding_id"), TEXT("GUID of the MVVM binding to remove (see list_mvvm_bindings)."), true);
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_RemoveMVVMBinding::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("remove_mvvm_binding"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	FString BindingIdStr;
	if (!Params->TryGetStringField(TEXT("binding_id"), BindingIdStr) || BindingIdStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: binding_id"));
	}

	FGuid BindingId;
	if (!FGuid::Parse(BindingIdStr, BindingId))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid binding_id GUID: '%s'"), *BindingIdStr));
	}

	UMVVMBlueprintView* View = ClaireonWidgetHelpers::GetOrCreateMVVMBlueprintView(WBP);
	if (!View)
	{
		return MakeErrorResult(TEXT("No MVVM Blueprint View exists"));
	}

	FMVVMBlueprintViewBinding* Binding = View->GetBinding(BindingId);
	if (!Binding)
	{
		return MakeErrorResult(FString::Printf(TEXT("Binding with id '%s' not found"), *BindingIdStr));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove MVVM Binding")));

	View->RemoveBinding(Binding);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("removed_binding_id"), BindingIdStr);

	return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Removed MVVM binding '%s'"), *BindingIdStr));
}

