// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TransactionEndGroup.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonTransactionGroupState.h"
#include "Editor.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_TransactionEndGroup::GetOperation() const { return TEXT("end_group"); }

FString ClaireonTool_TransactionEndGroup::GetDescription() const
{
	return TEXT("End the active transaction group. The group appears as a single entry in undo history.");
}

TSharedPtr<FJsonObject> ClaireonTool_TransactionEndGroup::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	return Builder.Build();
}

FToolResult ClaireonTool_TransactionEndGroup::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	if (!ClaireonTransactionGroupState::bGroupActive)
	{
		return MakeErrorResult(TEXT("No active group to end."));
	}

	GEditor->EndTransaction();

	const FString EndedLabel = ClaireonTransactionGroupState::ActiveGroupLabel;
	ClaireonTransactionGroupState::bGroupActive = false;
	ClaireonTransactionGroupState::ActiveGroupLabel.Empty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("group_ended"), true);

	return MakeSuccessResult(Result, FString::Printf(TEXT("Ended transaction group: %s"), *EndedLabel));
}
