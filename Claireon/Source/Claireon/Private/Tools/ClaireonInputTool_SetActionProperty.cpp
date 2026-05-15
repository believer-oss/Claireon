// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_SetActionProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "InputAction.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_SetActionProperty::GetName() const
{
	return TEXT("claireon.input_set_action_property");
}

FString ClaireonInputTool_SetActionProperty::GetDescription() const
{
	return TEXT("Set a property on the Input Action of this session (string-typed value).");
}

TSharedPtr<FJsonObject> ClaireonInputTool_SetActionProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("property_name"), TEXT("Property name on UInputAction to set."), true);
	Builder.AddString(TEXT("value"), TEXT("String value to assign."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_SetActionProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_action_property' requires 'property_name'"));
	}

	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("'set_action_property' requires 'value'"));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Input Action Property")));
	IA->Modify();

	if (!ClaireonEnhancedInputHelpers::SetObjectProperty(IA, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("set_action_property -- %s = %s"), *PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}
