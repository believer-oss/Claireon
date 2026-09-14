// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TransactionUndo.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonTransactionGroupState.h"
#include "Tools/ClaireonBPSnapshot.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"

using FToolResult = IClaireonTool::FToolResult;

// Require the full requested undo count; report zero or partial progress as an error.
// Read identities afterward because Undo can skip expired entries.
//
// Collapse undo can leave foreign-outer nodes that crash graph painting (see
// ClaireonBPEditorTransactionBaselines). This tool does not repair them: it cannot
// identify all touched graphs, and an untransacted repair would desynchronize redo.

namespace ClaireonTransactionUndoDetail
{
	/** Wire values for undo_failure_reason. */
	inline constexpr TCHAR ReasonNoBuffer[] = TEXT("no_transaction_buffer");
	inline constexpr TCHAR ReasonEmptyBuffer[] = TEXT("empty_buffer");
	inline constexpr TCHAR ReasonUndoFailed[] = TEXT("undo_failed");

	/** How many entries remain undoable, or 0 when there is no buffer to ask. */
	int32 RemainingUndoableCount()
	{
		const UTransBuffer* TransBuffer = IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
		if (!IsValid(TransBuffer))
		{
			return 0;
		}
		return FMath::Max(0, TransBuffer->GetQueueLength() - TransBuffer->GetUndoCount());
	}

	/** Use CanUndo: an open transaction is already in the queue but cannot be undone. */
	bool TransactorWillUndo(FString& OutBlockedReason)
	{
		OutBlockedReason.Reset();
		if (!IsValid(GEditor) || !IsValid(GEditor->Trans))
		{
			return false;
		}

		FText Why;
		if (GEditor->Trans->CanUndo(&Why))
		{
			return true;
		}
		OutBlockedReason = Why.ToString();
		return false;
	}

	/**
	 * Title and id of the transaction the undo just reversed.
	 *
	 * After Undo, UndoCount has been advanced past the entry that was applied, so that entry
	 * sits at QueueLength - UndoCount. bCanRedo defaults true, so the buffer is not truncated
	 * and this index is valid.
	 */
	void DescribeJustUndone(FString& OutTitle, FString& OutId)
	{
		OutTitle = TEXT("(unknown)");
		OutId.Reset();

		const UTransBuffer* TransBuffer = IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
		if (!IsValid(TransBuffer))
		{
			return;
		}

		const int32 QueueLength = TransBuffer->GetQueueLength();
		const int32 UndoneIndex = QueueLength - TransBuffer->GetUndoCount();
		if (UndoneIndex < 0 || UndoneIndex >= QueueLength)
		{
			return;
		}

		if (const FTransaction* Transaction = TransBuffer->GetTransaction(UndoneIndex))
		{
			OutTitle = Transaction->GetTitle().ToString();
			OutId = Transaction->GetId().ToString(EGuidFormats::DigitsWithHyphens);
		}
	}
}

FString ClaireonTool_TransactionUndo::GetOperation() const { return TEXT("undo"); }

FString ClaireonTool_TransactionUndo::GetDescription() const
{
	return TEXT("Undo the last N transactions and report what was actually reversed: requested_count, "
		"undone_count, titles and transaction ids. Reversing fewer than requested -- none included -- is "
		"an ERROR carrying undo_failure_reason and failed_attempt_index, never a success with a zero "
		"count. Stateless / non-session: drives the editor-wide transactor without opening any per-asset "
		"session.");
}

TSharedPtr<FJsonObject> ClaireonTool_TransactionUndo::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddInteger(TEXT("count"), TEXT("Number of transactions to undo (default 1)."));
	return Builder.Build();
}

FToolResult ClaireonTool_TransactionUndo::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonTransactionUndoDetail;

	if (!IsValid(GEditor))
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	int32 RequestedCount = 1;
	if (Arguments->HasField(TEXT("count")))
	{
		RequestedCount = FMath::Max(1, static_cast<int32>(Arguments->GetNumberField(TEXT("count"))));
	}

	TArray<TSharedPtr<FJsonValue>> UndoneTitles;
	TArray<TSharedPtr<FJsonValue>> UndoneIds;
	int32 UndoneCount = 0;

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

		if (!TransactorWillUndo(BlockedReason))
		{
			// Distinguish queue exhaustion from open transactions and undo barriers.
			FailureReason = RemainingUndoableCount() <= 0 ? ReasonEmptyBuffer : ReasonUndoFailed;
			FailedAttemptIndex = Attempt;
			break;
		}

		// Inject one failed attempt to exercise partial undo without corrupting the buffer.
		const bool bInjectedFailure =
			CLAIREON_BP_SHOULD_INJECT_FAILURE(*ClaireonTransactionFaultSeam::UndoAttempt(Attempt));

		if (bInjectedFailure || !GEditor->UndoTransaction())
		{
			FailureReason = ReasonUndoFailed;
			FailedAttemptIndex = Attempt;
			break;
		}

		++UndoneCount;

		FString Title;
		FString Id;
		DescribeJustUndone(Title, Id);
		UndoneTitles.Add(MakeShared<FJsonValueString>(Title));
		if (!Id.IsEmpty())
		{
			UndoneIds.Add(MakeShared<FJsonValueString>(Id));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("requested_count"), RequestedCount);
	Result->SetNumberField(TEXT("undone_count"), UndoneCount);
	Result->SetArrayField(TEXT("transactions"), UndoneTitles);
	Result->SetArrayField(TEXT("undone_transaction_ids"), UndoneIds);

	if (UndoneCount == RequestedCount)
	{
		return MakeSuccessResult(Result, FString::Printf(TEXT("Undid %d transaction(s)"), UndoneCount));
	}

	Result->SetStringField(TEXT("undo_failure_reason"), FailureReason);
	Result->SetNumberField(TEXT("failed_attempt_index"), FailedAttemptIndex);
	if (!BlockedReason.IsEmpty())
	{
		Result->SetStringField(TEXT("transactor_message"), BlockedReason);
	}

	const FString Message = UndoneCount == 0
		? FString::Printf(TEXT("Undid nothing: %s. Requested %d."), *FailureReason, RequestedCount)
		: FString::Printf(TEXT("Undid %d of %d transaction(s): attempt %d failed with %s. The "
			"reversals that succeeded are NOT rolled forward."),
			UndoneCount, RequestedCount, FailedAttemptIndex, *FailureReason);

	FToolResult Error = MakeErrorResult(Message);
	Error.Data = Result;
	return Error;
}
