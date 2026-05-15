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
    return TEXT("List MVVM bindings (source/destination paths, mode, conversion function) on the Widget Blueprint.");
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
    return Operation_ListMVVMBindings(SessionId, Data, Params);
}

// ============================================================================
// Operation body (relocated from ClaireonWidgetBPEditToolBase.cpp in stage 024)
// ============================================================================

FToolResult ClaireonWidgetBPEditToolBase::Operation_ListMVVMBindings(const FString& SessionId, FWidgetBPEditToolData* Data, const TSharedPtr<FJsonObject>& Params)
{
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
