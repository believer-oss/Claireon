// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_EditMVVMBinding.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Widget.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewBinding.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMPropertyPath.h"
#include "MVVMBlueprintViewConversionFunction.h"
#include "Types/MVVMBindingMode.h"
#include "Tools/ClaireonWidgetBPEditToolBase_Internal.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_EditMVVMBinding::GetOperation() const { return TEXT("edit_mvvm_binding"); }

FString ClaireonWidgetBPTool_EditMVVMBinding::GetDescription() const
{
    return TEXT("Edit an existing MVVM binding by id in the open Widget Blueprint editing session: mode, enabled, property paths, conversion function. Requires open session_id from widgetbp_open. Transactional. Omitted fields are left unchanged. Common pitfall: changing source path requires the viewmodel context to still be present.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_EditMVVMBinding::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("binding_id"), TEXT("GUID of the binding to edit (see list_mvvm_bindings)."), true);
    Builder.AddString(TEXT("mode"), TEXT("Optional new mode: 'OneWayToDestination', 'OneWayToSource', 'TwoWay', 'OneTimeToDestination', 'OneTimeToSource'."));
    Builder.AddBoolean(TEXT("enabled"), TEXT("Optional enabled flag."));
    Builder.AddString(TEXT("viewmodel_property"), TEXT("Optional new ViewModel property path."));
    Builder.AddString(TEXT("widget_property"), TEXT("Optional new widget property path."));
    Builder.AddString(TEXT("conversion_function"), TEXT("Optional conversion function; pass empty string to clear."));
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_EditMVVMBinding::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("edit_mvvm_binding"), Params, SessionId, Data, Error))
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

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Edit MVVM Binding")));

	// Optional: mode
	FString ModeStr;
	if (Params->TryGetStringField(TEXT("mode"), ModeStr) && !ModeStr.IsEmpty())
	{
		EMVVMBindingMode NewMode;
		if (!ClaireonWidgetBPInternal::ParseBindingMode(ModeStr, NewMode))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid mode: '%s'"), *ModeStr));
		}
		Binding->BindingType = NewMode;
	}

	// Optional: enabled
	bool bEnabled;
	if (Params->TryGetBoolField(TEXT("enabled"), bEnabled))
	{
		Binding->bEnabled = bEnabled;
	}

	// Optional: viewmodel_property
	FString ViewModelProperty;
	if (Params->TryGetStringField(TEXT("viewmodel_property"), ViewModelProperty) && !ViewModelProperty.IsEmpty())
	{
		// We need the ViewModel class from the source path
		FGuid VMId = Binding->SourcePath.GetViewModelId();
		const FMVVMBlueprintViewModelContext* VMContext = View->FindViewModel(VMId);
		if (!VMContext)
		{
			return MakeErrorResult(TEXT("Cannot update viewmodel_property: ViewModel context not found for this binding's source"));
		}
		UClass* VMClass = VMContext->GetViewModelClass();
		if (!VMClass)
		{
			return MakeErrorResult(TEXT("ViewModel class is null"));
		}

		FMVVMBlueprintPropertyPath NewSourcePath;
		NewSourcePath.SetViewModelId(VMId);
		FString PathError;
		if (!ClaireonWidgetBPInternal::ResolvePropertyPath(WBP, NewSourcePath, VMClass, ViewModelProperty, PathError))
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to resolve viewmodel property path '%s': %s"), *ViewModelProperty, *PathError));
		}
		Binding->SourcePath = NewSourcePath;
	}

	// Optional: widget_property
	FString WidgetProperty;
	if (Params->TryGetStringField(TEXT("widget_property"), WidgetProperty) && !WidgetProperty.IsEmpty())
	{
		FName WidgetName = Binding->DestinationPath.GetWidgetName();
		UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, WidgetName);
		if (!Widget)
		{
			return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found in the tree for property path update"), *WidgetName.ToString()));
		}

		FMVVMBlueprintPropertyPath NewDestPath;
		NewDestPath.SetWidgetName(WidgetName);
		FString PathError;
		if (!ClaireonWidgetBPInternal::ResolvePropertyPath(WBP, NewDestPath, Widget->GetClass(), WidgetProperty, PathError))
		{
			return MakeErrorResult(FString::Printf(TEXT("Failed to resolve widget property path '%s': %s"), *WidgetProperty, *PathError));
		}
		Binding->DestinationPath = NewDestPath;
	}

	// Optional: conversion_function (Stage 004)
	FString ConversionFunctionStr;
	if (Params->TryGetStringField(TEXT("conversion_function"), ConversionFunctionStr))
	{
		if (ConversionFunctionStr.IsEmpty())
		{
			// Clear conversion
			UMVVMBlueprintViewConversionFunction* Existing = Binding->Conversion.GetConversionFunction(/*bSourceToDestination=*/true);
			if (Existing)
			{
				Existing->RemoveWrapperGraph(WBP);
			}
			Binding->Conversion.SourceToDestinationConversion = nullptr;
		}
		else
		{
			FString ConvError;
			const UFunction* ConvFunc = ClaireonWidgetBPInternal::ResolveConversionFunction(WBP, ConversionFunctionStr, ConvError);
			if (!ConvFunc)
			{
				return MakeErrorResult(FString::Printf(TEXT("Failed to resolve conversion function '%s': %s"), *ConversionFunctionStr, *ConvError));
			}

			UMVVMBlueprintViewConversionFunction* ConvObj = NewObject<UMVVMBlueprintViewConversionFunction>(View);
			FName GraphName = MakeUniqueObjectName(WBP, UEdGraph::StaticClass(), TEXT("MVVM_Conv"));
			ConvObj->InitializeFromFunction(WBP, GraphName, ConvFunc);
			Binding->Conversion.SourceToDestinationConversion = ConvObj;
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = ClaireonWidgetHelpers::SerializeMVVMBinding(WBP, *Binding);
	return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Updated MVVM binding '%s'"), *BindingIdStr));
}

