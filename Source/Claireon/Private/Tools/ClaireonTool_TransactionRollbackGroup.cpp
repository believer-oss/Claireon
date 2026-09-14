// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#include "Tools/ClaireonTool_TransactionRollbackGroup.h"
#include "Tools/FToolSchemaBuilder.h"
#include "Tools/ClaireonTransactionGroupState.h"
#include "Tools/ClaireonBPSnapshot.h"
#include "Editor.h"
#include "Editor/TransBuffer.h"

using FToolResult = IClaireonTool::FToolResult;

// Report group closure separately from rollback success. EndTransaction runs
// before Undo, so a failed undo leaves no active group to retry; report whether
// the group remains at the undo head and ordinary undo is available.

namespace ClaireonTransactionRollbackDetail
{
	/** Wire values for rollback failures. */
	inline constexpr TCHAR ReasonNoBuffer[] = TEXT("no_transaction_buffer");
	inline constexpr TCHAR ReasonUndoFailed[] = TEXT("undo_failed");
	inline constexpr TCHAR ReasonNotAtUndoHead[] = TEXT("group_not_at_undo_head");
	inline constexpr TCHAR RefusalUnprovableSettlement[] = TEXT("formatting_settlement_unprovable");

	/** Use CanUndo to include blockers such as an open transaction, not just buffer presence. */
	bool OrdinaryUndoAvailable()
	{
		if (!IsValid(GEditor) || !IsValid(GEditor->Trans))
		{
			return false;
		}
		return GEditor->Trans->CanUndo();
	}

	/**
	 * After closing, compare the head with the captured transaction ID. Titles can
	 * repeat, and closing an empty group exposes the preceding transaction.
	 */
	bool GroupTransactionIsAtUndoHead(const FGuid& GroupTransactionId)
	{
		if (!GroupTransactionId.IsValid())
		{
			return false;
		}
		const UTransBuffer* TransBuffer = IsValid(GEditor) ? Cast<UTransBuffer>(GEditor->Trans) : nullptr;
		if (!IsValid(TransBuffer))
		{
			return false;
		}

		const int32 HeadIndex = TransBuffer->GetQueueLength() - TransBuffer->GetUndoCount() - 1;
		if (HeadIndex < 0 || HeadIndex >= TransBuffer->GetQueueLength())
		{
			return false;
		}

		const FTransaction* Transaction = TransBuffer->GetTransaction(HeadIndex);
		return Transaction && Transaction->GetContext().TransactionId == GroupTransactionId;
	}
}

FString ClaireonTool_TransactionRollbackGroup::GetOperation() const { return TEXT("rollback_group"); }

FString ClaireonTool_TransactionRollbackGroup::GetDescription() const
{
	return TEXT("Cancel the active transaction group and undo it, reporting group_closed, "
		"group_rolled_back, label and undo_available as SEPARATE facts. Refuses by default and retains "
		"the group, because pending third-party formatting cannot be proven settled; pass "
		"acknowledge_unsettled_formatting to take that risk yourself. Stateless / non-session: aborts a "
		"transaction scope on the editor-wide transactor.");
}

TSharedPtr<FJsonObject> ClaireonTool_TransactionRollbackGroup::GetInputSchema() const
{
	FToolSchemaBuilder Builder;
	Builder.AddBoolean(TEXT("acknowledge_unsettled_formatting"),
		TEXT("Default false, which REFUSES and retains the group. Set true only when you know no "
		     "third-party formatting is pending on the graphs the group touched: the tool cannot "
		     "establish that itself, and rolling back under pending formatting formats an "
		     "already-reverted graph. transaction_end_group is the safe alternative."));
	return Builder.Build();
}

// Refuse rollback without explicit acknowledgement: delayed BlueprintAssist work
// can format an already-reverted graph. PendingFormatting is private, stable
// positions do not prove settlement, and the group records no graph set to settle.
// Pumping Slate could also capture unrelated work in the open transaction.
//
// A missing formatting transaction is not evidence of quiet: auto-formatting may
// be disabled and empty records are discarded. Acknowledgement permits the attempt
// without claiming product-verified safety; rollback_group_safe remains false.
FToolResult ClaireonTool_TransactionRollbackGroup::Execute(const TSharedPtr<FJsonObject>& Arguments)
{
	using namespace ClaireonTransactionRollbackDetail;

	if (!IsValid(GEditor))
	{
		return MakeErrorResult(TEXT("GEditor is not available"));
	}

	if (!ClaireonTransactionGroupState::bGroupActive)
	{
		return MakeErrorResult(TEXT("No active group to rollback."));
	}

	const FString Label = ClaireonTransactionGroupState::ActiveGroupLabel;
	const FString GroupTitle = ClaireonTransactionGroupState::MakeGroupTitle(Label);

	bool bAcknowledged = false;
	Arguments->TryGetBoolField(TEXT("acknowledge_unsettled_formatting"), bAcknowledged);

	if (!bAcknowledged)
	{
		// Keep the group open on refusal.
		TSharedPtr<FJsonObject> RefusalData = MakeShared<FJsonObject>();
		RefusalData->SetBoolField(TEXT("group_closed"), false);
		RefusalData->SetBoolField(TEXT("group_rolled_back"), false);
		RefusalData->SetBoolField(TEXT("group_retained"), true);
		RefusalData->SetStringField(TEXT("label"), GroupTitle);
		RefusalData->SetBoolField(TEXT("rollback_group_safe"), false);
		RefusalData->SetStringField(TEXT("refusal_reason"), RefusalUnprovableSettlement);
		RefusalData->SetBoolField(TEXT("undo_available"), OrdinaryUndoAvailable());

		// Keep essential facts in top-level scalars so they survive spilling.
		TSharedPtr<FJsonObject> Evidence = MakeShared<FJsonObject>();
		Evidence->SetStringField(TEXT("detection"),
			TEXT("BlueprintAssist's PendingFormatting is private with no accessor on the stock "
			     "public surface, and stable node positions are not proof -- its delayed "
			     "graph-change detector can still be waiting to open a transaction."));
		Evidence->SetStringField(TEXT("population"),
			TEXT("The group records no graph population: ClaireonTransactionGroupState holds only "
			     "bGroupActive, ActiveGroupLabel and the group transaction's id, so a bounded "
			     "settle would have no defined target, and the group is editor-wide while "
			     "formatting follows the active graph handler."));
		Evidence->SetStringField(TEXT("not_evidence"),
			TEXT("The absence of a 'Format Node Added' transaction is NOT treated as evidence "
			     "here: this project checks in bGloballyDisableAutoFormatting=True, so that "
			     "transaction can never appear and its absence would clear a guard on a codebase "
			     "where the hazard was never handled at all."));
		RefusalData->SetObjectField(TEXT("refusal_evidence"), Evidence);

		FToolResult Refusal = MakeErrorResult(FString::Printf(
			TEXT("Refused to roll back group '%s': the tool cannot prove no third-party formatting "
			     "is pending on the graphs it touched. The group is RETAINED and still open."),
			*GroupTitle));
		Refusal.Data = RefusalData;
		Refusal.AddHint(MakeGuidanceHint(
			TEXT("transaction_end_group"),
			TEXT("The group is still open and editor-wide, so every later edit -- yours and anyone "
			     "else's -- is swept into it; doing nothing is not a terminal state. "
			     "transaction_end_group closes it as one undo record and is the safe action. Roll "
			     "back only by re-calling with acknowledge_unsettled_formatting when you know the "
			     "affected graphs are quiet.")));
		return Refusal;
	}

	// Close before undo; even a failed undo leaves the group closed.
	GEditor->EndTransaction();

	const bool bTransactionAtUndoHead =
		GroupTransactionIsAtUndoHead(ClaireonTransactionGroupState::ActiveGroupTransactionId);
	const bool bHasBuffer = IsValid(GEditor->Trans);

	// Inject failure to exercise the recovery report.
	const bool bInjectedFailure = CLAIREON_BP_SHOULD_INJECT_FAILURE(ClaireonTransactionFaultSeam::RollbackGroupUndo);

	// Only undo the group's own transaction. Closing an empty group exposes unrelated prior work.
	const bool bRolledBack =
		!bInjectedFailure && bTransactionAtUndoHead && GEditor->UndoTransaction();

	ClaireonTransactionGroupState::bGroupActive = false;
	ClaireonTransactionGroupState::ActiveGroupLabel.Empty();
	ClaireonTransactionGroupState::ActiveGroupTransactionId.Invalidate();

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetBoolField(TEXT("group_closed"), true);
	Result->SetBoolField(TEXT("group_rolled_back"), bRolledBack);
	Result->SetStringField(TEXT("label"), GroupTitle);
	Result->SetBoolField(TEXT("undo_available"), OrdinaryUndoAvailable());
	Result->SetBoolField(TEXT("rollback_group_safe"), false);
	Result->SetBoolField(TEXT("safety_acknowledged_by_caller"), true);

	if (bRolledBack)
	{
		return MakeSuccessResult(Result, FString::Printf(
			TEXT("Rolled back transaction group: %s"), *GroupTitle));
	}

	const TCHAR* FailureReason = !bHasBuffer ? ReasonNoBuffer
		: (!bTransactionAtUndoHead ? ReasonNotAtUndoHead : ReasonUndoFailed);
	Result->SetBoolField(TEXT("transaction_at_undo_head"), bTransactionAtUndoHead);
	Result->SetStringField(TEXT("undo_failure_reason"), FailureReason);

	const FString ErrorText = bTransactionAtUndoHead
		? FString::Printf(
			TEXT("Group '%s' was CLOSED but NOT rolled back: the underlying undo failed (%s). The "
			     "group no longer exists and cannot be retried as a group rollback."),
			*GroupTitle, FailureReason)
		: FString::Printf(
			TEXT("Group '%s' was CLOSED but NOT rolled back: no undo was attempted (%s). The "
			     "pending undo is not this group's transaction -- a group that recorded no "
			     "changes is popped on close -- so undoing would have reversed an earlier, "
			     "unrelated edit. The group no longer exists and cannot be retried as a group "
			     "rollback."),
			*GroupTitle, FailureReason);
	FToolResult Error = MakeErrorResult(ErrorText);
	Error.Data = Result;

	if (bTransactionAtUndoHead)
	{
		Error.AddHint(MakeGuidanceHint(
			TEXT("transaction_undo"),
			TEXT("The group is closed and its transaction is still at the head of the ordinary undo "
			     "buffer, so transaction_undo targets it next and is the recommended retry. It is "
			     "one undo record: count=1 reverses the whole group.")));
	}
	else
	{
		Error.AddHint(MakeGuidanceHint(
			TEXT("bp_get_graph"),
			TEXT("The group is closed and its transaction is NOT at the head of the ordinary undo "
			     "buffer -- it either never reached the queue (a group that recorded no changes is "
			     "popped) or something else sits above it. Do NOT undo blindly: that reverts an "
			     "unrelated transaction. Inspect the current state and repair forward.")));
	}
	return Error;
}
