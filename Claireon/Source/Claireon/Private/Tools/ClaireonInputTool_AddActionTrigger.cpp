// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_AddActionTrigger.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonEnhancedInputHelpers.h"
#include "InputAction.h"
#include "InputTriggers.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_AddActionTrigger::GetOperation() const { return TEXT("add_action_trigger"); }

FString ClaireonInputTool_AddActionTrigger::GetDescription() const
{
	return TEXT("Add a trigger instance to the Input Action of this session.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_AddActionTrigger::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddString(TEXT("trigger_class"), TEXT("Trigger class name (e.g. InputTriggerPressed)."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_AddActionTrigger::Execute(const TSharedPtr<FJsonObject>& Arguments)
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

	FString TriggerClassName;
	if (!Arguments->TryGetStringField(TEXT("trigger_class"), TriggerClassName) || TriggerClassName.IsEmpty())
	{
		return MakeErrorResult(TEXT("'add_action_trigger' requires 'trigger_class'"));
	}

	UClass* TriggerClass = ClaireonEnhancedInputHelpers::ResolveTriggerClass(TriggerClassName, Error);
	if (!TriggerClass)
	{
		return MakeErrorResult(Error);
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Add Input Action Trigger")));
	IA->Modify();

	UInputTrigger* NewTrigger = ClaireonEnhancedInputHelpers::CreateTrigger(IA, TriggerClass);
	if (!NewTrigger)
	{
		return MakeErrorResult(TEXT("Failed to create trigger instance"));
	}

	const int32 NewIndex = IA->Triggers.Add(NewTrigger);

	Data->LastOperationStatus = FString::Printf(TEXT("add_action_trigger -- Added %s at index %d"), *TriggerClass->GetName(), NewIndex);
	return BuildStateResponse(SessionId, Data);
}
