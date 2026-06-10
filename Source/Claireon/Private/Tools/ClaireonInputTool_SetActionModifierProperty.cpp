// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_SetActionModifierProperty.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "InputAction.h"
#include "InputModifiers.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_SetActionModifierProperty::GetOperation() const { return TEXT("set_action_modifier_property"); }

FString ClaireonInputTool_SetActionModifierProperty::GetDescription() const
{
    return TEXT("Set a property on a specific modifier of the Input Action. Session-mode tool: open via input_open first.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_SetActionModifierProperty::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("index"), TEXT("Modifier index on UInputAction."), true);
	Builder.AddString(TEXT("property_name"), TEXT("Property name on the modifier."), true);
	Builder.AddString(TEXT("value"), TEXT("String value to assign."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_SetActionModifierProperty::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
		return MakeErrorResult(TEXT("'set_action_modifier_property' requires 'index'"));
	}

	if (Index < 0 || Index >= IA->Modifiers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Modifier index %d out of range (0-%d)"), Index, IA->Modifiers.Num() - 1));
	}

	FString PropertyName;
	if (!Arguments->TryGetStringField(TEXT("property_name"), PropertyName) || PropertyName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'set_action_modifier_property' requires 'property_name'"));
	}

	FString Value;
	if (!Arguments->TryGetStringField(TEXT("value"), Value))
	{
		return MakeErrorResult(TEXT("'set_action_modifier_property' requires 'value'"));
	}

	UInputModifier* Modifier = IA->Modifiers[Index];
	if (!Modifier)
	{
		return MakeErrorResult(FString::Printf(TEXT("Modifier at index %d is null"), Index));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Set Input Action Modifier Property")));
	IA->Modify();
	Modifier->Modify();

	if (!ClaireonEnhancedInputHelpers::SetObjectProperty(Modifier, PropertyName, Value, Error))
	{
		return MakeErrorResult(Error);
	}

	Data->LastOperationStatus = FString::Printf(TEXT("set_action_modifier_property -- [%d].%s = %s"), Index, *PropertyName, *Value);
	return BuildStateResponse(SessionId, Data);
}
