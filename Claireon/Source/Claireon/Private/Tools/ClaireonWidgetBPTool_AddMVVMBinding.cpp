// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_AddMVVMBinding.h"
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

FString ClaireonWidgetBPTool_AddMVVMBinding::GetOperation() const { return TEXT("add_mvvm_binding"); }

FString ClaireonWidgetBPTool_AddMVVMBinding::GetDescription() const
{
    return TEXT("Add an MVVM binding between a ViewModel property and a widget property in the open Widget Blueprint editing session. Requires open session_id from widgetbp_open. Transactional. The viewmodel context must already be added via widgetbp_add_mvvm_viewmodel before bindings can resolve their source property paths.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_AddMVVMBinding::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("viewmodel_name"), TEXT("Name of the ViewModel context (must already exist; see add_mvvm_viewmodel)."), true);
    Builder.AddString(TEXT("viewmodel_property"), TEXT("Dot-separated property path on the ViewModel."), true);
    Builder.AddString(TEXT("widget_name"), TEXT("Name of the widget in the widget tree."), true);
    Builder.AddString(TEXT("widget_property"), TEXT("Dot-separated property path on the widget."), true);
    Builder.AddString(TEXT("mode"), TEXT("Binding mode: 'OneWayToDestination' (default), 'OneWayToSource', 'TwoWay', 'OneTimeToDestination', 'OneTimeToSource'."));
    Builder.AddBoolean(TEXT("enabled"), TEXT("Whether the binding is enabled (default true)."));
    Builder.AddString(TEXT("conversion_function"), TEXT("Optional conversion function path, 'Class::Function', or self-context function name."));
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_AddMVVMBinding::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("add_mvvm_binding"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	// Required params
	FString ViewModelName, ViewModelProperty, WidgetNameStr, WidgetProperty;
	if (!Params->TryGetStringField(TEXT("viewmodel_name"), ViewModelName) || ViewModelName.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: viewmodel_name"));
	}
	if (!Params->TryGetStringField(TEXT("viewmodel_property"), ViewModelProperty) || ViewModelProperty.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: viewmodel_property"));
	}
	if (!Params->TryGetStringField(TEXT("widget_name"), WidgetNameStr) || WidgetNameStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: widget_name"));
	}
	if (!Params->TryGetStringField(TEXT("widget_property"), WidgetProperty) || WidgetProperty.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: widget_property"));
	}

	// Optional params
	FString ModeStr = TEXT("OneWayToDestination");
	Params->TryGetStringField(TEXT("mode"), ModeStr);
	EMVVMBindingMode BindingMode;
	if (!ClaireonWidgetBPInternal::ParseBindingMode(ModeStr, BindingMode))
	{
		return MakeErrorResult(FString::Printf(TEXT("Invalid mode: '%s'. Valid: OneWayToDestination, OneWayToSource, TwoWay, OneTimeToDestination, OneTimeToSource"), *ModeStr));
	}

	bool bEnabled = true;
	Params->TryGetBoolField(TEXT("enabled"), bEnabled);

	FString ConversionFunctionStr;
	Params->TryGetStringField(TEXT("conversion_function"), ConversionFunctionStr);

	// Validate MVVM view exists
	UMVVMBlueprintView* View = ClaireonWidgetHelpers::GetOrCreateMVVMBlueprintView(WBP);
	if (!View)
	{
		return MakeErrorResult(TEXT("Failed to get or create MVVM Blueprint View"));
	}

	// Find ViewModel context
	const FMVVMBlueprintViewModelContext* VMContext = View->FindViewModel(FName(*ViewModelName));
	if (!VMContext)
	{
		return MakeErrorResult(FString::Printf(TEXT("ViewModel '%s' not found. Add it first with add_mvvm_viewmodel."), *ViewModelName));
	}

	// Validate widget exists
	UWidget* Widget = ClaireonWidgetHelpers::FindWidgetByName(WBP->WidgetTree, FName(*WidgetNameStr));
	if (!Widget)
	{
		return MakeErrorResult(FString::Printf(TEXT("Widget '%s' not found in the widget tree"), *WidgetNameStr));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add MVVM Binding")));

	FMVVMBlueprintViewBinding& NewBinding = View->AddDefaultBinding();

	// Configure source path (ViewModel)
	FMVVMBlueprintPropertyPath SourcePath;
	SourcePath.SetViewModelId(VMContext->GetViewModelId());
	{
		FString PathError;
		UClass* VMClass = VMContext->GetViewModelClass();
		if (!VMClass)
		{
			View->RemoveBinding(&NewBinding);
			return MakeErrorResult(TEXT("ViewModel class is null"));
		}
		if (!ClaireonWidgetBPInternal::ResolvePropertyPath(WBP, SourcePath, VMClass, ViewModelProperty, PathError))
		{
			View->RemoveBinding(&NewBinding);
			return MakeErrorResult(FString::Printf(TEXT("Failed to resolve viewmodel property path '%s': %s"), *ViewModelProperty, *PathError));
		}
	}
	NewBinding.SourcePath = SourcePath;

	// Configure destination path (Widget)
	FMVVMBlueprintPropertyPath DestPath;
	DestPath.SetWidgetName(FName(*WidgetNameStr));
	{
		FString PathError;
		if (!ClaireonWidgetBPInternal::ResolvePropertyPath(WBP, DestPath, Widget->GetClass(), WidgetProperty, PathError))
		{
			View->RemoveBinding(&NewBinding);
			return MakeErrorResult(FString::Printf(TEXT("Failed to resolve widget property path '%s': %s"), *WidgetProperty, *PathError));
		}
	}
	NewBinding.DestinationPath = DestPath;

	NewBinding.BindingType = BindingMode;
	NewBinding.bEnabled = bEnabled;
	NewBinding.bCompile = true;

	// Stage 004: Conversion function support
	if (!ConversionFunctionStr.IsEmpty())
	{
		FString ConvError;
		const UFunction* ConvFunc = ClaireonWidgetBPInternal::ResolveConversionFunction(WBP, ConversionFunctionStr, ConvError);
		if (!ConvFunc)
		{
			View->RemoveBinding(&NewBinding);
			return MakeErrorResult(FString::Printf(TEXT("Failed to resolve conversion function '%s': %s"), *ConversionFunctionStr, *ConvError));
		}

		UMVVMBlueprintViewConversionFunction* ConvObj = NewObject<UMVVMBlueprintViewConversionFunction>(View);
		FName GraphName = MakeUniqueObjectName(WBP, UEdGraph::StaticClass(), TEXT("MVVM_Conv"));
		ConvObj->InitializeFromFunction(WBP, GraphName, ConvFunc);
		NewBinding.Conversion.SourceToDestinationConversion = ConvObj;
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	TSharedPtr<FJsonObject> ResultObj = ClaireonWidgetHelpers::SerializeMVVMBinding(WBP, NewBinding);
	return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Added MVVM binding: %s.%s -> %s.%s"), *ViewModelName, *ViewModelProperty, *WidgetNameStr, *WidgetProperty));
}

