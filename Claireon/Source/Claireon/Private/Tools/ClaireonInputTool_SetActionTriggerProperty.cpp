// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_SetActionTriggerProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "InputAction.h"
#include "InputTriggers.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_SetActionTriggerProperty::GetName() const
{
	return TEXT("claireon.input_set_action_trigger_property");
}

FString ClaireonInputTool_SetActionTriggerProperty::GetDescription() const
{
	return TEXT("Set a property on a specific trigger of the Input Action.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_SetActionTriggerProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("index"), TEXT("Trigger index on UInputAction."), true);
	Builder.AddString(TEXT("property_name"), TEXT("Property name on the trigger."), true);
	Builder.AddString(TEXT("value"), TEXT("String value to assign."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_SetActionTriggerProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	int32 Index = -1;
	if (!Arguments->TryGetNumberField(TEXT("index"), Index))
	{
		return MakeErrorResult(TEXT("'set_action_trigger_property' requires 'index'"));
	}

	if (Index < 0 || Index >= IA->Triggers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Trigger index %d out of range (0-%d)"), Index, IA->Triggers.Num() - 1));
	}

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_action_trigger_property' requires 'property_name'"));
	}

	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("'set_action_trigger_property' requires 'value'"));
	}

	UInputTrigger* Trigger = IA->Triggers[Index];
	if (!Trigger)
	{
		return MakeErrorResult(FString::Printf(TEXT("Trigger at index %d is null"), Index));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Input Action Trigger Property")));
	IA->Modify();
	Trigger->Modify();

	if (!ClaireonEnhancedInputHelpers::SetObjectProperty(Trigger, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("set_action_trigger_property -- [%d].%s = %s"), Index, *PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}
