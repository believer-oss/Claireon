// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_ListMVVMViewModels.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_ListMVVMViewModels::GetName() const
{
    return TEXT("claireon.widgetbp_list_mvvm_viewmodels");
}

FString ClaireonWidgetBPTool_ListMVVMViewModels::GetDescription() const
{
    return TEXT("List ViewModel contexts currently registered on the Widget Blueprint's MVVM extension.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_ListMVVMViewModels::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_ListMVVMViewModels::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("list_mvvm_viewmodels"), Params, SessionId, Data, Error))
    {
        return Error;
    }
    return Operation_ListMVVMViewModels(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_ListMVVMViewModels(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	TSharedPtr<FJsonObject> Result = ClaireonWidgetHelpers::SerializeMVVMViewModelContexts(WBP);
	int32 Count = 0;
	if (Result.IsValid())
	{
		Count = static_cast<int32>(Result->GetNumberField(TEXT("count")));
	}

	return MakeSuccessResult(Result, FString::Printf(TEXT("Found %d MVVM ViewModel context(s)"), Count));
}
