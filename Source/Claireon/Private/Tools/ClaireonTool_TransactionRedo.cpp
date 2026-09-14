// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TransactionRedo.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonTransactionGroupState.h"
#include "Tools/ClaireonBPSnapshot.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"

using FToolResult = IClaireonTool::FToolResult;

// Require the full requested redo count; report zero or partial progress as an error.
// Read identities after each redo because the transactor can skip expired entries.

namespace ClaireonTransactionRedoDetail
{
	inline constexpr TCHAR ReasonNoBuffer[] = TEXT("no_transaction_buffer");
	inline constexpr TCHAR ReasonEmptyRedoStack[] = TEXT("empty_redo_stack");
	inline constexpr TCHAR ReasonRedoFailed[] = TEXT("redo_failed");

	/** How many entries remain redoable, or 0 when there is no buffer to ask. */
	int32 RemainingRedoableCount()
	{
		const UTransBuffer* TransBuffer = IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
		if (!IsValid(TransBuffer))
		{
			return 0;
		}
		return FMath::Max(0, TransBuffer->GetUndoCount());
	}

	/** Use CanRedo to detect blockers such as open transactions, not just queue exhaustion. */
	bool TransactorWillRedo(FString& OutBlockedReason)
	{
		OutBlockedReason.Reset();
		if (!IsValid(GEditor) || !IsValid(GEditor->Trans))
		{
			return false;
		}

		FText Why;
		if (GEditor->Trans->CanRedo(&Why))
		{
			return true;
		}
		OutBlockedReason = Why.ToString();
		return false;
	}

	/**
	 * Title and id of the transaction the redo just reapplied.
	 *
	 * Redo decrements UndoCount past the entry it applied, so that entry sits at
	 * QueueLength - UndoCount - 1 afterwards.
	 */
	void DescribeJustRedone(FString& OutTitle, FString& OutId)
	{
		OutTitle = TEXT("(unknown)");
		OutId.Reset();

		const UTransBuffer* TransBuffer = IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
		if (!IsValid(TransBuffer))
		{
			return;
		}

		const int32 QueueLength = TransBuffer->GetQueueLength();
		const int32 RedoneIndex = QueueLength - TransBuffer->GetUndoCount() - 1;
		if (RedoneIndex < 0 || RedoneIndex >= QueueLength)
		{
			return;
		}

		if (const FTransaction* Transaction = TransBuffer->GetTransaction(RedoneIndex))
		{
			OutTitle = Transaction->GetTitle().ToString();
			OutId = Transaction->GetId().ToString(EGuidFormats::DigitsWithHyphens);
		}
	}
}

FString ClaireonTool_TransactionRedo::GetOperation() const { return TEXT("redo"); }

FString ClaireonTool_TransactionRedo::GetDescription() const
{
	return TEXT("Redo the last N undone transactions and report what was actually reapplied: "
		"requested_count, redone_count, titles and transaction ids. Reapplying fewer than requested -- "
		"none included -- is an ERROR carrying redo_failure_reason and failed_attempt_index, never a "
		"success with a zero count. Stateless / non-session: drives the editor-wide transactor without "
		"opening any session.");
}

TSharedPtr<FJsonObject> ClaireonTool_TransactionRedo::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddInteger(TEXT("count"), TEXT("Number of transactions to redo (default 1)."));
	return Builder.Build();
}

FToolResult ClaireonTool_TransactionRedo::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonTransactionRedoDetail;

	if (!IsValid(GEditor))
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	int32 RequestedCount = 1;
	if (Arguments->HasField(TEXT("count")))
	{
		RequestedCount = FMath::Max(1, static_cast<int32>(Arguments->GetNumberField(TEXT("count"))));
	}

	TArray<TSharedPtr<FJsonValue>> RedoneTitles;
	TArray<TSharedPtr<FJsonValue>> RedoneIds;
	int32 RedoneCount = 0;

	FString FailureReason;
	FString BlockedReason;
	int32 FailedAttemptIndex = INDEX_NONE;

	for (int32 Attempt = 0; Attempt < RequestedCount; ++Attempt)
	{
		if (!IsValid(GEditor->Trans))
		{
			FailureReason = ReasonNoBuffer;
			FailedAttemptIndex = Attempt;
			break;
		}

		if (!TransactorWillRedo(BlockedReason))
		{
			// Distinguish an exhausted redo stack from other transactor refusals.
			FailureReason = RemainingRedoableCount() <= 0 ? ReasonEmptyRedoStack : ReasonRedoFailed;
			FailedAttemptIndex = Attempt;
			break;
		}

		const bool bInjectedFailure =
			CLAIREON_BP_SHOULD_INJECT_FAILURE(*ClaireonTransactionFaultSeam::RedoAttempt(Attempt));

		if (bInjectedFailure || !GEditor->RedoTransaction())
		{
			FailureReason = ReasonRedoFailed;
			FailedAttemptIndex = Attempt;
			break;
		}

		++RedoneCount;

		FString Title;
		FString Id;
		DescribeJustRedone(Title, Id);
		RedoneTitles.Add(MakeShared<FJsonValueString>(Title));
		if (!Id.IsEmpty())
		{
			RedoneIds.Add(MakeShared<FJsonValueString>(Id));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("requested_count"), RequestedCount);
	Result->SetNumberField(TEXT("redone_count"), RedoneCount);
	Result->SetArrayField(TEXT("transactions"), RedoneTitles);
	Result->SetArrayField(TEXT("redone_transaction_ids"), RedoneIds);

	if (RedoneCount == RequestedCount)
	{
		return MakeSuccessResult(Result, FString::Printf(TEXT("Redid %d transaction(s)"), RedoneCount));
	}

	Result->SetStringField(TEXT("redo_failure_reason"), FailureReason);
	Result->SetNumberField(TEXT("failed_attempt_index"), FailedAttemptIndex);
	if (!BlockedReason.IsEmpty())
	{
		Result->SetStringField(TEXT("transactor_message"), BlockedReason);
	}

	const FString Message = RedoneCount == 0
		? FString::Printf(TEXT("Redid nothing: %s. Requested %d."), *FailureReason, RequestedCount)
		: FString::Printf(TEXT("Redid %d of %d transaction(s): attempt %d failed with %s. The "
			"reapplications that succeeded are NOT rolled back."),
			RedoneCount, RequestedCount, FailedAttemptIndex, *FailureReason);

	FToolResult Error = MakeErrorResult(Message);
	Error.Data = Result;
	return Error;
}
