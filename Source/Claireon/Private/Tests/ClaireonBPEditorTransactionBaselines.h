// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

// Transaction characterization baselines after explicit BA settlement.
// Update sequences and supported BA versions together after re-characterization; engine identity is recorded only.
// Composite extraction allows fixture reset by removing ubergraph nodes.

#include "CoreMinimal.h"

#if WITH_CLAIREON_TESTS && WITH_EDITOR

namespace ClaireonBPEditorBaselines
{
	// ---- Provenance -------------------------------------------------------------------

	/** BA versions with verified matching sequences. Versions with different sequences need separate baselines. */
	inline const TCHAR* const BlueprintAssistVersionNames[] = { TEXT("4.5.2"), TEXT("4.9.1") };

	/** Engine build identity at characterization time. Recorded and reported, not asserted. */
	inline const TCHAR* const EngineBuildVersion = TEXT("UE5-CL-0");

	/** Which suite characterized these, so a later reader knows what owns them. */
	inline const TCHAR* const CharacterizedBy =
		TEXT("the Claireon.BPEditor transaction-baseline suite");

	/** An uncharacterized sentinel must fail rather than pass as an empty baseline. */
	inline const TCHAR* const Uncharacterized = TEXT("<UNCHARACTERIZED>");

	// With auto-formatting disabled, node-added transactions record no changes and are discarded.
	// Settlement can move nodes without a surviving transaction, so observe positions independently.
	// No delayed transaction in this configuration does not prove pending BA work was handled.

	/** Transactions surviving into the queue from a settle after a node add. Measured: none. */
	inline constexpr int32 SettleAfterNodeAddTransactionCount = 0;

	/** Whether a node-added transaction survives settlement in this configuration. */
	inline constexpr bool bFormatNodeAddedTransactionSurvives = false;

	// Ungrouped extraction: observe the queue suffix after the effective undo position, then settle BA.

	inline const TCHAR* const UngroupedExtractTransactions[] =
	{
		TEXT("Extract composite"),
	};

	// Grouped extraction and explicit test-only settlement produce one nested transaction.
	// This does not establish production rollback timing.

	inline const TCHAR* const GroupedExtractTransactions[] =
	{
		TEXT("[Claireon] gate1b-4b-grouped-extract"),
	};

	/** transaction_undo(count=1) after the group closed. */
	inline constexpr int32 GroupedUndoExpectedUndoneCount = 1;

	/** Whether one undo removes the composite gateway; node counts cannot distinguish replacement from restoration. */
	inline constexpr bool bGroupedUndoRemovesTheCompositeGateway = true;

	/**
	 * Nodes listed in the ubergraph whose Outer was not restored by undo.
	 * Such inconsistency can fault a later SGraphPanel paint.
	 */
	inline constexpr int32 GroupedUndoForeignOuterNodeCount = 1;
}

#endif // WITH_CLAIREON_TESTS && WITH_EDITOR
