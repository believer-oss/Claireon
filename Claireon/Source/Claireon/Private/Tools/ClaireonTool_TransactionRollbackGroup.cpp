// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TransactionRollbackGroup.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonTransactionGroupState.h"
#include "Editor.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_TransactionRollbackGroup::GetName() const
{
	return TEXT("claireon.transaction_rollback_group");
}

FString ClaireonTool_TransactionRollbackGroup::GetDescription() const
{
	return TEXT("Cancel the active transaction group and undo all operations within it.");
}

TSharedPtr<FJsonObject> ClaireonTool_TransactionRollbackGroup::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	return Builder.Build();
}

FToolResult ClaireonTool_TransactionRollbackGroup::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	if (!ClaireonTransactionGroupState::bGroupActive)
	{
		return MakeErrorResult(TEXT("No active group to rollback."));
	}

	const FString RolledBackLabel = ClaireonTransactionGroupState::ActiveGroupLabel;

	// Close the group first, then undo it.
	GEditor->EndTransaction();
	GEditor->UndoTransaction();

	ClaireonTransactionGroupState::bGroupActive = false;
	ClaireonTransactionGroupState::ActiveGroupLabel.Empty();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("group_rolled_back"), true);

	return MakeSuccessResult(Result, FString::Printf(TEXT("Rolled back transaction group: %s"), *RolledBackLabel));
}
