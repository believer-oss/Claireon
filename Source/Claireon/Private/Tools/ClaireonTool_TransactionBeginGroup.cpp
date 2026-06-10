// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TransactionBeginGroup.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonTransactionGroupState.h"
#include "Editor.h"

using FToolResult = IClaireonTool::FToolResult;

FString ClaireonTool_TransactionBeginGroup::GetOperation() const { return TEXT("begin_group"); }

FString ClaireonTool_TransactionBeginGroup::GetDescription() const
{
	return TEXT("Start a transaction group. All subsequent tool calls are grouped into a single undo step. "
		"Call transaction_end_group to finalize or transaction_rollback_group to cancel.");
}

TSharedPtr<FJsonObject> ClaireonTool_TransactionBeginGroup::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddString(TEXT("label"),
		TEXT("Descriptive label for the group. The [Claireon] prefix is prepended automatically."),
		/*bRequired=*/true);
	return Builder.Build();
}

FToolResult ClaireonTool_TransactionBeginGroup::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	if (!GEditor)
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	if (ClaireonTransactionGroupState::bGroupActive)
	{
		return MakeErrorResult(FString::Printf(
			TEXT("A group is already active: '%s'. Call transaction_end_group or transaction_rollback_group first."),
			*ClaireonTransactionGroupState::ActiveGroupLabel));
	}

	FString Label;
	if (!Arguments->TryGetStringField(TEXT("label"), Label) || Label.IsEmpty())
	{
		return MakeErrorResult(TEXT("Missing required parameter: label"));
	}

	const FString FullLabel = FString::Printf(TEXT("[Claireon] %s"), *Label);
	GEditor->BeginTransaction(FText::FromString(FullLabel));

	ClaireonTransactionGroupState::bGroupActive = true;
	ClaireonTransactionGroupState::ActiveGroupLabel = Label;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("group_started"), true);
	Result->SetStringField(TEXT("label"), FullLabel);

	return MakeSuccessResult(Result, FString::Printf(TEXT("Started transaction group: %s"), *FullLabel));
}
