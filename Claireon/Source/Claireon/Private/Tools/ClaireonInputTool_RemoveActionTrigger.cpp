// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_RemoveActionTrigger.h"
#include "Tools/FToolSchemaBuilder.h"
#include "InputAction.h"
#include "InputTriggers.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_RemoveActionTrigger::GetName() const
{
	return TEXT("claireon.input_remove_action_trigger");
}

FString ClaireonInputTool_RemoveActionTrigger::GetDescription() const
{
	return TEXT("Remove a trigger at the given index from the Input Action of this session.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_RemoveActionTrigger::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("index"), TEXT("Trigger index to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_RemoveActionTrigger::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
		return MakeErrorResult(TEXT("'remove_action_trigger' requires 'index'"));
	}

	if (Index < 0 || Index >= IA->Triggers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Trigger index %d out of range (0-%d)"), Index, IA->Triggers.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Input Action Trigger")));
	IA->Modify();

	FString RemovedName = IA->Triggers[Index] ? IA->Triggers[Index]->GetClass()->GetName() : TEXT("(null)");
	IA->Triggers.RemoveAt(Index);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_action_trigger -- Removed %s from index %d"), *RemovedName, Index);
	return BuildStateResponse(SessionId, Data);
}
