// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_AddActionModifier.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "InputAction.h"
#include "InputModifiers.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_AddActionModifier::GetName() const
{
	return TEXT("claireon.input_add_action_modifier");
}

FString ClaireonInputTool_AddActionModifier::GetDescription() const
{
	return TEXT("Add a modifier instance to the Input Action of this session.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_AddActionModifier::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("modifier_class"), TEXT("Modifier class name (e.g. InputModifierDeadZone)."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_AddActionModifier::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString ModifierClassName;
	if (!Arguments->TryGetStringField(TEXT("modifier_class"), ModifierClassName) || ModifierClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'add_action_modifier' requires 'modifier_class'"));
	}

	UClass* ModifierClass = ClaireonEnhancedInputHelpers::ResolveModifierClass(ModifierClassName, Error);
	if (!ModifierClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Input Action Modifier")));
	IA->Modify();

	UInputModifier* NewModifier = ClaireonEnhancedInputHelpers::CreateModifier(IA, ModifierClass);
	if (!NewModifier)
	{
		return MakeErrorResult(TEXT("Failed to create modifier instance"));
	}

	const int32 NewIndex = IA->Modifiers.Add(NewModifier);

	Data->LastOperationStatus = FString::Printf(TEXT("add_action_modifier -- Added %s at index %d"), *ModifierClass->GetName(), NewIndex);
	return BuildStateResponse(SessionId, Data);
}
