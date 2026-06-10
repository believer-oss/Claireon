// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

/**
 * Shared process-wide transaction-group state.
 *
 * Two independently-tracked transaction scopes:
 *
 *   1. Auto-transaction (bAutoTransactionActive) - opened automatically by
 *      ClaireonTool_ExecutePython at the start of every python_execute call.
 *      Committed on success; cancelled (discarded) on error.  The LLM can
 *      discard it early by calling claireon.transaction_undo() from within the
 *      script; transaction_undo checks this flag before deciding whether to
 *      do a regular Ctrl+Z or discard the in-flight auto-transaction.
 *
 *   2. Explicit group (bGroupActive) - opened by ClaireonTool_TransactionBeginGroup
 *      (no longer registered as a Python-callable tool after the auto-transaction
 *      was introduced; kept for C++ callers and forward compatibility).
 *
 * FClaireonServer is single-instance and single-session, so a file-scope
 * namespace singleton is sufficient.  Module shutdown calls ResetGroupState()
 * to close any leaked open scopes.
 */
namespace ClaireonTransactionGroupState
{
	// --- Explicit group (legacy; begin_group tool no longer registered) ---
	extern CLAIREON_API bool bGroupActive;
	extern CLAIREON_API FString ActiveGroupLabel;

	// --- Auto-transaction (python_execute wrapper) ---
	/** True while a python_execute call has an open auto-transaction. */
	extern CLAIREON_API bool bAutoTransactionActive;
	/** Index returned by GEditor->BeginTransaction; used for CancelTransaction. */
	extern CLAIREON_API int32 AutoTransactionIndex;

	/** Close and discard any open auto-transaction then any open explicit group.
	 *  Called at module shutdown and server reset. */
	CLAIREON_API void ResetGroupState();

	/** Discard the open auto-transaction via EndTransaction+UndoTransaction.
	 *  No-op when no auto-transaction is active.  Called by transaction_undo
	 *  when the LLM wants to roll back the current python_execute scope. */
	CLAIREON_API void DiscardAutoTransaction();
}
