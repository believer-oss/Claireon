// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_SetValueType.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_SetValueType::GetOperation() const { return TEXT("set_value_type"); }

FString ClaireonInputTool_SetValueType::GetDescription() const
{
    return TEXT("Set the value type on an Input Action session (bool, float, 2d, 3d). Session-mode tool: open via input_open first.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_SetValueType::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("value_type"), TEXT("Value type (bool, float, 2d, 3d)."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_SetValueType::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	FString SessionId;
	FInputEditToolData* Data = nullptr;
	FString Error;
	if (!RequireSession(Arguments, SessionId, Data, Error))
	{
		return MakeErrorResult(Error);
	}

	UInputAction* IA = RequireInputAction(Data, Error);
	if (!IA)
	{
		return MakeErrorResult(Error);
	}

	FString ValueTypeStr;
	if (!Arguments->TryGetStringField(TEXT("value_type"), ValueTypeStr) || ValueTypeStr.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_value_type' requires 'value_type' (bool, float, 2d, 3d)"));
	}

	EInputActionValueType NewType;
	if (!ClaireonEnhancedInputHelpers::ParseValueType(ValueTypeStr, NewType, Error))
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Input Action Value Type")));
	IA->Modify();
	IA->ValueType = NewType;

	Data->LastOperationStatus = FString::Printf(TEXT("set_value_type -- %s"), *ClaireonEnhancedInputHelpers::ValueTypeToString(NewType));
	return BuildStateResponse(SessionId, Data);
}
