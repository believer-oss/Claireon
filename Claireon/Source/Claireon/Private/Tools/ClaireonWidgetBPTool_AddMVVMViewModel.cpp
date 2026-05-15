// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_AddMVVMViewModel.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonNameResolver.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "ScopedTransaction.h"
#include "MVVMBlueprintView.h"
#include "MVVMBlueprintViewModelContext.h"
#include "MVVMViewModelBase.h"
#include "Tools/ClaireonWidgetBPEditToolBase_Internal.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_AddMVVMViewModel::GetName() const
{
    return TEXT("claireon.widgetbp_add_mvvm_viewmodel");
}

FString ClaireonWidgetBPTool_AddMVVMViewModel::GetDescription() const
{
    return TEXT("Add an MVVM ViewModel context to the Widget Blueprint.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_AddMVVMViewModel::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    Builder.AddString(TEXT("viewmodel_name"), TEXT("Unique name for the ViewModel context."), true);
    Builder.AddString(TEXT("viewmodel_class"), TEXT("UMVVMViewModelBase subclass name or asset path."), true);
    Builder.AddString(TEXT("creation_type"), TEXT("Creation mode: 'Manual' (default), 'CreateInstance', 'GlobalViewModelCollection', 'PropertyPath', 'Resolver'."));
    Builder.AddBoolean(TEXT("optional"), TEXT("Whether the ViewModel is optional (default false)."));
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_AddMVVMViewModel::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("add_mvvm_viewmodel"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_AddMVVMViewModel(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_AddMVVMViewModel(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
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

	FString ViewModelClassStr;
	if (!Params->TryGetStringField(TEXT("viewmodel_class"), ViewModelClassStr) || ViewModelClassStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required param: viewmodel_class"));
	}

	// Resolve viewmodel class
	ClaireonNameResolver::FNameResolveResult VMClassResult;
	UClass* VMClass = ClaireonNameResolver::ResolveClassName(ViewModelClassStr, UMVVMViewModelBase::StaticClass(), VMClassResult);
	if (!VMClass)
	{
		// Fall back to LoadClass for asset paths
		VMClass = LoadClass<UMVVMViewModelBase>(nullptr, *ViewModelClassStr);
	}
	if (!VMClass || !VMClass->IsChildOf(UMVVMViewModelBase::StaticClass()))
	{
		return MakeErrorResult(FString::Printf(TEXT("Could not resolve '%s' to a UMVVMViewModelBase subclass"), *ViewModelClassStr));
	}

	// Parse optional params
	FString CreationTypeStr;
	EMVVMBlueprintViewModelContextCreationType CreationType = EMVVMBlueprintViewModelContextCreationType::Manual;
	if (Params->TryGetStringField(TEXT("creation_type"), CreationTypeStr) && !CreationTypeStr.IsEmpty())
	{
		if (!ClaireonWidgetBPInternal::ParseCreationType(CreationTypeStr, CreationType))
		{
			return MakeErrorResult(FString::Printf(TEXT("Invalid creation_type: '%s'. Valid values: Manual, CreateInstance, GlobalViewModelCollection, PropertyPath, Resolver"), *CreationTypeStr));
		}
	}

	bool bOptional = false;
	Params->TryGetBoolField(TEXT("optional"), bOptional);

	// Get or create MVVM view
	UMVVMBlueprintView* View = ClaireonWidgetHelpers::GetOrCreateMVVMBlueprintView(WBP);
	if (!View)
	{
		return MakeErrorResult(TEXT("Failed to get or create MVVM Blueprint View"));
	}

	// Check for duplicate
	if (View->FindViewModel(FName(*ViewModelName)) != nullptr)
	{
		return MakeErrorResult(FString::Printf(TEXT("ViewModel with name '%s' already exists"), *ViewModelName));
	}

	// Construct and add context
	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add MVVM ViewModel")));

	FMVVMBlueprintViewModelContext Context;
	Context.ViewModelName = FName(*ViewModelName);
	Context.NotifyFieldValueClass = VMClass;
	Context.CreationType = CreationType;
	Context.bOptional = bOptional;

	View->AddViewModel(Context);

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(WBP);
	Data->bModified = true;

	// Return the created context info
	TSharedPtr<FJsonObject> ResultObj = MakeShared<FJsonObject>();
	ResultObj->SetStringField(TEXT("viewmodel_name"), ViewModelName);
	ResultObj->SetStringField(TEXT("viewmodel_class"), VMClass->GetPathName());
	ResultObj->SetStringField(TEXT("creation_type"), CreationTypeStr.IsEmpty() ? TEXT("Manual") : *CreationTypeStr);
	ResultObj->SetBoolField(TEXT("optional"), bOptional);

	// Try to get the assigned GUID
	const FMVVMBlueprintViewModelContext* Added = View->FindViewModel(FName(*ViewModelName));
	if (Added)
	{
		ResultObj->SetStringField(TEXT("id"), Added->GetViewModelId().ToString());
	}

	return MakeSuccessResult(ResultObj, FString::Printf(TEXT("Added MVVM ViewModel '%s' (%s)"), *ViewModelName, *VMClass->GetName()));
}
