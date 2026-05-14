// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonWidgetBPTool_ListMVVMBindings.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Dom/JsonObject.h"
#include "ClaireonWidgetHelpers.h"
#include "WidgetBlueprint.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonWidgetBPTool_ListMVVMBindings::GetName() const
{
    return TEXT("claireon.widgetbp_list_mvvm_bindings");
}

FString ClaireonWidgetBPTool_ListMVVMBindings::GetDescription() const
{
    return TEXT("List MVVM bindings on the Widget Blueprint in the open editing session: source/destination paths, mode, conversion function, and binding id. Requires open session_id from claireon.widgetbp_open. Read-only. Returns one entry per binding; the id is the handle for claireon.widgetbp_edit_mvvm_binding and remove.");
}

TSharedPtr<FJsonObject> ClaireonWidgetBPTool_ListMVVMBindings::GetInputSchema() const
{
    FToolSchemaBuilder Builder;
    Builder.AddSessionParams();
    return Builder.Build();
}

FToolResult ClaireonWidgetBPTool_ListMVVMBindings::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
    TSharedPtr<FJsonObject> Params;
    FString SessionId;
    FWidgetBPEditToolData* Data = nullptr;
    FToolResult Error;
    if (!BeginSessionOp(Arguments, TEXT("list_mvvm_bindings"), Params, SessionId, Data, Error))
    {
        return Error;
    }
	UWidgetBlueprint* WBP = Data->WidgetBlueprint.Get();
	if (!WBP)
	{
		return MakeErrorResult(TEXT("Widget Blueprint is no longer valid"));
	}

	TSharedPtr<FJsonObject> Result = ClaireonWidgetHelpers::SerializeMVVMBindings(WBP);
	int32 Count = 0;
	if (Result.IsValid())
	{
		Count = static_cast<int32>(Result->GetNumberField(TEXT("count")));
	}

	return MakeSuccessResult(Result, FString::Printf(TEXT("Found %d MVVM binding(s)"), Count));
}

