// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

/**
 * Process-wide explicit transaction group, opened and closed by transaction tools.
 * The group combines all editor mutations into one undo entry, including user edits;
 * keep it short. ResetGroupState closes leaked groups at shutdown or server reset.
 */
namespace ClaireonTransactionGroupState
{
	/** True between transaction_begin_group and whichever tool closes the group. */
	extern CLAIREON_API bool bGroupActive;

	/** The caller's label, WITHOUT the [Claireon] prefix. Empty when no group is active. */
	extern CLAIREON_API FString ActiveGroupLabel;

	/**
	 * TransactionId captured from GUndo when the group opens; invalid when inactive.
	 * Rollback must match this identity, not a reusable label, to avoid undoing an older group.
	 */
	extern CLAIREON_API FGuid ActiveGroupTransactionId;

	/** Display title: caller label with [Claireon] prefix. Use ActiveGroupTransactionId for identity. */
	CLAIREON_API FString MakeGroupTitle(const FString& Label);

	/**
	 * Close an open group without undoing it at shutdown or server reset.
	 * Its record may no longer be at the undo head.
	 */
	CLAIREON_API void ResetGroupState();
}

// Test-only fault keys for rollback and indexed undo/redo attempts.
#if WITH_CLAIREON_TESTS
namespace ClaireonTransactionFaultSeam
{
	/** Arm to make transaction_rollback_group's UndoTransaction() report failure. */
	inline constexpr TCHAR RollbackGroupUndo[] = TEXT("transaction_rollback_group:undo");

	/** Arm to make the AttemptIndex'th (0-based) undo in transaction_undo report failure. */
	inline FString UndoAttempt(int32 AttemptIndex)
	{
		return FString::Printf(TEXT("transaction_undo:%d"), AttemptIndex);
	}

	/** Arm to make the AttemptIndex'th (0-based) redo in transaction_redo report failure. */
	inline FString RedoAttempt(int32 AttemptIndex)
	{
		return FString::Printf(TEXT("transaction_redo:%d"), AttemptIndex);
	}
}
#endif // WITH_CLAIREON_TESTS
