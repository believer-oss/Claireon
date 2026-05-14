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
    return TEXT("List ViewModel contexts currently registered on the Widget Blueprint's MVVM extension in the open editing session. Requires open session_id from claireon.widgetbp_open. Read-only. Returns one entry per viewmodel context with class and name; manage via claireon.widgetbp_add_mvvm_viewmodel and remove counterparts.");
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

