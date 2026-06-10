// Copyright (c) 2026 The Claireon Contributors
// SPDX-License-Identifier: MIT

#pragma once

#include "CoreMinimal.h"

class UChooserTable;
struct FInstancedStruct;

/**
 * Shared graph traversal helpers for chooser tools.
 *
 * Why these exist: the previous implementations of chooser_walk and
 * chooser_find_rows discovered sub-choosers via UChooserTable::NestedChoosers,
 * which is a flat editor-only registry of all sub-objects in the asset (not
 * just direct children). That made depth tracking wrong (everything appeared
 * at depth 1 from root) and conflated reachable nodes with orphaned dead
 * sub-choosers. The helpers here traverse via row-result references
 * (FNestedChooser / FEvaluateChooser) which is the true asset structure;
 * NestedChoosers is consulted only for orphan detection.
 */
namespace ClaireonChooserGraphHelpers
{
	/** Returns the sub-chooser referenced by a row's result, or nullptr if the
	 *  row's result is a leaf (asset / soft-asset / proxy / unset). */
	CLAIREON_API UChooserTable* GetRowSubChooser(UChooserTable* Chooser, int32 RowIndex);

	/** Per-chooser BFS over the row-result graph rooted at Root. Visits each
	 *  chooser exactly once with correct depth + (parent_path, parent_row_index)
	 *  attribution. Visitor returning false stops the traversal early.
	 *  MaxDepth: -1 = unbounded, 0 = root only, N = include nodes at depth <= N. */
	using FNodeVisitor = TFunction<bool(UChooserTable* Chooser, int32 Depth, const FString& ParentPath, int32 ParentRowIndex)>;
	CLAIREON_API void EnumerateChoosersBFS(UChooserTable* Root, FNodeVisitor Visitor, int32 MaxDepth = -1);

	/** Per-row DFS — for each chooser visited, calls Visitor on every row in
	 *  order; when a row's result is a sub-chooser ref, recurses into that
	 *  sub-chooser's rows before continuing the parent. Use this for
	 *  "row-by-row what does this dispatcher chain do" queries.
	 *  Visitor returning false stops the traversal early.
	 *  MaxDepth: -1 = unbounded, 0 = root rows only, N = recurse to depth <= N. */
	using FRowVisitor = TFunction<bool(UChooserTable* Chooser, int32 RowIndex, int32 Depth, const FString& ParentPath, int32 ParentRowIndex)>;
	CLAIREON_API void TraverseRowsDFS(UChooserTable* Root, FRowVisitor Visitor, int32 MaxDepth = -1);

	/** Identify NestedChoosers entries (anywhere in the asset's flat registry)
	 *  that are NOT reachable via row-result traversal from Root. These are
	 *  dead sub-choosers — created at some point but no row ever points to them.
	 *  ReachablePaths should be populated via EnumerateChoosersBFS first. */
	CLAIREON_API TArray<UChooserTable*> CollectOrphans(UChooserTable* Root, const TSet<FString>& ReachablePaths);
}
