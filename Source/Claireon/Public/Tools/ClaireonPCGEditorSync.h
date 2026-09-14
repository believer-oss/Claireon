// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UPCGGraph;

/** Outcome of an attempt to refresh the editor graph for a PCG graph asset. */
enum class EPCGReconstructResult : uint8
{
	/** The editor graph was rebuilt from the runtime graph. */
	Reconstructed,
	/** No asset editor is open and no editor graph is cached, so there is nothing stale. */
	NothingToRefresh,
	/**
	 * A rebuild is needed, but reflected classes/properties are unavailable or replacement
	 * node construction fails. The existing view is unchanged; never reported as success.
	 */
	Unavailable,
};

/** What an already-open (or previously-opened) PCG asset editor is showing. */
enum class EPCGEditorViewState : uint8
{
	/** No cached editor graph; opening the asset builds one from the runtime graph. */
	NoEditorGraph,
	/** A cached editor graph exists without an open window; reopening reuses it. */
	CachedButClosed,
	/** An asset editor is open right now, showing that same cached editor graph. */
	EditorOpen,
};

/** Synchronize cached PCG editor graphs after runtime-graph edits. */
namespace ClaireonPCGEditorSync
{
	/**
	 * True when this engine exposes the reflected PCG editor node class and its PCGNode
	 * property under the names the rebuild is written against.
	 */
	CLAIREON_API bool IsReconstructAvailable();

	/** Return the cached-graph/window state. Game thread only. */
	CLAIREON_API EPCGEditorViewState GetEditorViewState(UPCGGraph* InGraph);

	/** One-line, caller-facing explanation of a view state, including what to do about it. */
	CLAIREON_API FString DescribeViewState(EPCGEditorViewState State);

	/**
	 * Reconcile editor nodes, pins, and links with the runtime graph. NothingToRefresh
	 * means no cached graph exists; Unavailable leaves the existing view unchanged.
	 * Resolve classes and construct replacements before removing any nodes.
	 * Game thread only; must run outside a transaction.
	 */
	CLAIREON_API EPCGReconstructResult ReconstructOpenEditor(UPCGGraph* InGraph);

	/**
	 * Same rebuild for graphs that reference InGraph (one level of referencers), so editing a
	 * subgraph refreshes the parent graph the user is actually looking at. Only already-loaded
	 * assets are considered; nothing is loaded as a side effect of an edit. Returns the number
	 * of parent editor graphs rebuilt. Bounded by MaxParentsPerFlush.
	 */
	CLAIREON_API int32 ReconstructOpenParentEditors(UPCGGraph* InGraph);

	/**
	 * Queue a structural refresh for the next tick, coalescing requests per graph
	 * and running after the caller's FScopedTransaction closes.
	 */
	CLAIREON_API void RequestReconstruct(UPCGGraph* InGraph);

	/** Drop any pending request for InGraph (used when a caller handles it immediately). */
	CLAIREON_API void CancelPendingReconstruct(UPCGGraph* InGraph);

	/**
	 * Flush pending rebuilds if InGraph has one queued. Call before edits whose native
	 * notifications need newly added peer nodes and pins; a batch may not tick between edits.
	 * Game thread only; call outside a transaction.
	 */
	CLAIREON_API void SettlePendingReconstruct(UPCGGraph* InGraph);

	/** Unregister the pending-flush ticker. Called from module shutdown. */
	CLAIREON_API void ShutdownReconstructScheduler();

	/** Maximum parent editor graphs rebuilt per flush; skipped packages are logged. */
	inline constexpr int32 MaxParentsPerFlush = 16;

	/** Observability for tests. */
	struct FReconstructStats
	{
		/** Number of times the coalescing flush ran. */
		int32 FlushCount = 0;
		/** Number of graphs drained across all flushes. */
		int32 GraphsFlushed = 0;
		/** Number of editor graphs actually rebuilt. */
		int32 Reconstructed = 0;
		/** Number of graphs found with a stale editor view. */
		int32 StaleViewsDetected = 0;
	};

	CLAIREON_API const FReconstructStats& GetStats();
	CLAIREON_API void ResetStats();

	/** Number of graphs currently waiting for the next flush. */
	CLAIREON_API int32 GetPendingCount();

	/** Run the pending flush synchronously instead of waiting for the next tick (tests). */
	CLAIREON_API void FlushPendingNow();

	/**
	 * Test seam: make the named PCG editor node class resolve as absent, so the preflight
	 * failure path of ReconstructOpenEditor can be exercised against a real editor graph.
	 * NAME_None clears it. Never set outside a test.
	 */
	CLAIREON_API void SetMissingEditorNodeClassForTests(FName ClassName);
} // namespace ClaireonPCGEditorSync
