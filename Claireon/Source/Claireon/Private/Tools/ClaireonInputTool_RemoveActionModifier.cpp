// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonInputTool_RemoveActionModifier.h"
#include "Tools/FToolSchemaBuilder.h"
#include "InputAction.h"
#include "InputModifiers.h"
#include "ScopedTransaction.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonInputTool_RemoveActionModifier::GetName() const
{
	return TEXT("claireon.input_remove_action_modifier");
}

FString ClaireonInputTool_RemoveActionModifier::GetDescription() const
{
	return TEXT("Remove a modifier at the given index from the Input Action of this session.");
}

TSharedPtr<FJsonObject> ClaireonInputTool_RemoveActionModifier::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddSessionParams();
	Builder.AddInteger(TEXT("index"), TEXT("Modifier index to remove."), true);
	return Builder.Build();
}

FToolResult ClaireonInputTool_RemoveActionModifier::Execute(const TSharedPtr<FJsonObject>& Arguments)
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
		return MakeErrorResult(TEXT("'remove_action_modifier' requires 'index'"));
	}

	if (Index < 0 || Index >= IA->Modifiers.Num())
	{
		return MakeErrorResult(FString::Printf(TEXT("Modifier index %d out of range (0-%d)"), Index, IA->Modifiers.Num() - 1));
	}

	FScopedTransaction Transaction(FText::FromString(TEXT("[Claireon] Remove Input Action Modifier")));
	IA->Modify();

	FString RemovedName = IA->Modifiers[Index] ? IA->Modifiers[Index]->GetClass()->GetName() : TEXT("(null)");
	IA->Modifiers.RemoveAt(Index);

	Data->LastOperationStatus = FString::Printf(TEXT("remove_action_modifier -- Removed %s from index %d"), *RemovedName, Index);
	return BuildStateResponse(SessionId, Data);
}
